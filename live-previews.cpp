/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Scott Moreau
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <wayfire/plugin.hpp>
#include <wayfire/opengl.hpp>
#include <wayfire/render-manager.hpp>
#include <wayfire/view-transform.hpp>
#include <wayfire/scene-operations.hpp>
#include <wayfire/signal-definitions.hpp>
#include <wayfire/nonstd/wlroots-full.hpp>
#include <wayfire/plugins/ipc/ipc-activator.hpp>

extern "C" {
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#include <drm_fourcc.h>
}

namespace wf
{
namespace live_previews
{
struct live_preview
{
    wf::dimensions_t size;
    wf::output_t *output;
    wayfire_view view;
};

class simple_node_render_instance_t : public wf::scene::render_instance_t
{
    wf::signal::connection_t<wf::scene::node_damage_signal> on_node_damaged =
        [=] (wf::scene::node_damage_signal *ev)
    {
        push_to_parent(ev->region);
    };

    wf::scene::node_t *self;
    wf::scene::damage_callback push_to_parent;
    live_preview *preview;

  public:
    simple_node_render_instance_t(wf::scene::node_t *self, wf::scene::damage_callback push_dmg,
        live_preview *preview)
    {
        this->self    = self;
        this->preview = preview;
        this->push_to_parent = push_dmg;
        self->connect(&on_node_damaged);
    }

    void schedule_instructions(
        std::vector<wf::scene::render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::region_t& damage) override
    {
        instructions.push_back(wf::scene::render_instruction_t{
                    .instance = this,
                    .target   = target,
                    .damage   = damage,
                });
    }

    void render(const wf::scene::render_instruction_t& data) override
    {
        auto dst = data.target;

        preview->output->render->damage_whole();

        wf::gles::run_in_context([&]
        {
            if (!preview->view || !wf::toplevel_cast(preview->view))
            {
                return;
            }

            wf::auxilliary_buffer_t aux_buffer;
            auto vg = wf::toplevel_cast(preview->view)->get_geometry();
            auto current_scale = preview->output->handle->scale;

            if (vg.width > vg.height)
            {
                preview->output->handle->scale = std::min(1.0, preview->size.width / double(vg.width));
            } else
            {
                preview->output->handle->scale = std::min(1.0, preview->size.height / double(vg.height));
            }

            preview->view->take_snapshot(aux_buffer);
            preview->output->handle->scale = current_scale;
            auto src_size = aux_buffer.get_size();
            wf::gles::bind_render_buffer(dst);
            dst.blit(aux_buffer, wlr_fbox{0, 0, float(src_size.width), float(src_size.height)},
                wf::geometry_t{0, 0, preview->size.width, preview->size.height});
            aux_buffer.free();
        });
    }
};


class simple_node_t : public wf::scene::node_t
{
  public:
    live_preview *preview;
    simple_node_t(live_preview *preview) : wf::scene::node_t(false)
    {
        this->preview = preview;
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_damage, wf::output_t *shown_on) override
    {
        instances.push_back(std::make_unique<simple_node_render_instance_t>(
            this, push_damage, preview));
    }

    void do_push_damage(wf::region_t updated_region)
    {
        wf::scene::node_damage_signal ev;
        ev.region = updated_region;
        this->emit(&ev);
    }

    wf::geometry_t get_bounding_box() override
    {
        return {0, 0, preview->size.width, preview->size.height};
    }
};

std::shared_ptr<simple_node_t> add_simple_node(live_preview *preview)
{
    auto subnode = std::make_shared<simple_node_t>(preview);
    wf::scene::add_front(preview->output->node_for_layer(wf::scene::layer::LOCK), subnode);
    return subnode;
}

class live_previews_plugin : public wf::plugin_interface_t
{
    wf::option_wrapper_t<int> max_dimension{"live-previews/max_dimension"};
    std::shared_ptr<simple_node_t> live_preview_render_node;
    wf::wl_listener_wrapper on_session_active;
    wf::wl_timer<true> damage_timer;
    wf::wl_idle_call idle_damage;
    live_preview preview;

  private:
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> method_repository;
    std::unique_ptr<wf::scene::render_instance_manager_t> instance_manager = nullptr;
    wlr_backend *headless_backend = NULL;

    wf::scene::damage_callback push_damage = [=] (wf::region_t region)
    {
        if (!preview.output)
        {
            return;
        }

        region += -wf::origin(preview.output->get_layout_geometry());
        region  =
            preview.output->render->get_target_framebuffer().framebuffer_region_from_geometry_region(region);
        preview.output->render->damage(region, true);
    };

    void destroy_render_instance_manager()
    {
        if (!instance_manager)
        {
            return;
        }

        instance_manager.reset();
        instance_manager = nullptr;
    }

    void create_render_instance_manager(wayfire_view view)
    {
        if (instance_manager)
        {
            return;
        }

        std::vector<wf::scene::node_ptr> nodes;
        nodes.push_back(view->get_root_node());
        instance_manager = std::make_unique<wf::scene::render_instance_manager_t>(nodes, push_damage,
            view->get_output());
        instance_manager->set_visibility_region(view->get_output()->get_layout_geometry());
    }

  public:

    void init() override
    {
        method_repository->register_method("live_previews/request_stream", request_stream);
        method_repository->register_method("live_previews/release_output", release_output);
        on_session_active.set_callback([=] (void*)
        {
            if (!wf::get_core().session->active)
            {
                destroy_output();
            }
        });
        if (wf::get_core().session)
        {
            on_session_active.connect(&wf::get_core().session->events.active);
        }

        preview =
        {
            .size   = {0, 0},
            .output = nullptr,
            .view   = nullptr,
        };
    }

    wf::ipc::method_callback request_stream = [=] (wf::json_t data)
    {
        auto id = wf::ipc::json_get_uint64(data, "id");
        if (auto view = wf::ipc::find_view_by_id(id))
        {
            auto toplevel = wf::toplevel_cast(view);
            if (!toplevel)
            {
                return wf::ipc::json_error("view is not a toplevel");
            }

            auto vg = toplevel->get_geometry();
            if (vg.width < vg.height)
            {
                vg.width  = vg.width * (max_dimension / float(vg.height));
                vg.height = max_dimension;
            } else
            {
                vg.height = vg.height * (max_dimension / float(vg.width));
                vg.width  = max_dimension;
            }

            if ((vg.width != preview.size.width) || (vg.height != preview.size.height))
            {
                preview.size.width  = vg.width;
                preview.size.height = vg.height;
                if (preview.output)
                {
                    wlr_output_state state;
                    wlr_output_state_init(&state);
                    wlr_output_state_set_custom_mode(&state, vg.width, vg.height, 0);
                    if (wlr_output_test_state(preview.output->handle, &state))
                    {
                        wlr_output_commit_state(preview.output->handle, &state);
                    } else
                    {
                        destroy_output();
                    }
                }
            }

            if (preview.output)
            {
                view->connect(&view_unmapped);
                destroy_render_instance_manager();
                create_render_instance_manager(view);
                view->get_output()->render->damage_whole();
                preview.output->render->damage_whole();
                preview.view = view;
                view->damage();
                idle_damage.run_once([=] ()
                {
                    view->get_output()->render->damage_whole();
                    preview.output->render->damage_whole();
                    view->damage();
                });
                return wf::ipc::json_ok();
            }

            if (!headless_backend)
            {
                headless_backend = wlr_headless_backend_create(wf::get_core().ev_loop);
                wlr_multi_backend_add(wf::get_core().backend, headless_backend);
                wlr_backend_start(headless_backend);
            }

            auto handle = wlr_headless_add_output(headless_backend, vg.width, vg.height);
            wlr_output_state state;
            wlr_output_state_init(&state);
            wlr_output_state_set_render_format(&state, DRM_FORMAT_ABGR8888);
            if (wlr_output_test_state(handle, &state))
            {
                wlr_output_commit_state(handle, &state);
            }

            auto global = handle->global;
            handle->global = NULL;
            wlr_output_set_name(handle, "live-preview");
            wlr_output_set_description(handle, "Live Window Previews Virtual Output");
            handle->global = global;
            preview.output = wf::get_core().output_layout->find_output(handle);

            live_preview_render_node = add_simple_node(&preview);
            view->connect(&view_unmapped);
            destroy_render_instance_manager();
            create_render_instance_manager(view);
            view->get_output()->render->damage_whole();
            preview.output->render->damage_whole();
            preview.view = view;
            view->damage();
            idle_damage.run_once([=] ()
            {
                view->get_output()->render->damage_whole();
                preview.output->render->damage_whole();
                view->damage();
            });

            return wf::ipc::json_ok();
        }

        return wf::ipc::json_error("no such view");
    };

    wf::ipc::method_callback release_output = [=] (wf::json_t data)
    {
        destroy_render_instance_manager();
        view_unmapped.disconnect();

        return wf::ipc::json_ok();
    };

    wf::signal::connection_t<wf::view_unmapped_signal> view_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        if (ev->view != preview.view)
        {
            return;
        }

        destroy_output();
    };

    void destroy_output()
    {
        auto output = wf::get_core().output_layout->find_output("live-preview");
        if (!output)
        {
            return;
        }

        destroy_render_instance_manager();
        preview.view = nullptr;
        view_unmapped.disconnect();

        if (wf::get_core().seat->get_active_output() == output)
        {
            wf::get_core().seat->focus_output(
                wf::get_core().output_layout->get_next_output(output));
        }

        live_preview_render_node.reset();
        live_preview_render_node = nullptr;
        wlr_output_layout_remove(wf::get_core().output_layout->get_handle(), output->handle);
        wlr_output_destroy(output->handle);

        if (output == preview.output)
        {
            preview.output = nullptr;
        }
    }

    void fini() override
    {
        method_repository->unregister_method("live_previews/request_stream");
        method_repository->unregister_method("live_previews/release_output");
        destroy_output();
        on_session_active.disconnect();
    }
};
}
}

DECLARE_WAYFIRE_PLUGIN(wf::live_previews::live_previews_plugin);
