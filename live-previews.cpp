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
    double down_scale;
};

class simple_node_render_instance_t : public wf::scene::render_instance_t
{
    wf::signal::connection_t<wf::scene::node_damage_signal> on_node_damaged =
        [=] (wf::scene::node_damage_signal *ev)
    {
        push_to_parent(ev->region);
    };

    wf::scene::damage_callback push_damage = [=] (wf::region_t region)
    {
        if (!output)
        {
            return;
        }

        region += -wf::origin(preview->view->get_bounding_box());
        region  =
            output->render->get_target_framebuffer().framebuffer_region_from_geometry_region(region);
        output->render->damage(region, true);
    };

    std::unique_ptr<wf::scene::render_instance_manager_t> instance_manager = nullptr;
    wf::scene::damage_callback push_to_parent;
    live_preview *preview;
    wf::output_t *output;

  public:
    simple_node_render_instance_t(wf::scene::node_t *self, wf::scene::damage_callback push_dmg,
        live_preview *preview)
    {
        this->preview = preview;
        this->push_to_parent = push_dmg;
        self->connect(&on_node_damaged);
        LOGI("simple_node_render_instance_t");
        this->output = preview->output;
        std::vector<wf::scene::node_ptr> nodes;
        nodes.push_back(preview->view->get_root_node());
        instance_manager = std::make_unique<wf::scene::render_instance_manager_t>(nodes, push_damage, output);
        instance_manager->set_visibility_region(preview->view->get_bounding_box());
    }

    ~simple_node_render_instance_t()
    {
        LOGI("~simple_node_render_instance_t");
        instance_manager.reset();
        instance_manager = nullptr;
    }

    void schedule_instructions(
        std::vector<wf::scene::render_instruction_t>& instructions,
        const wf::render_target_t& target, wf::region_t& damage) override
    {
        auto offset = wf::origin(preview->view->get_bounding_box());
        damage *= 1.0 / preview->down_scale;
        damage += offset;
        auto transformed_target = target.translated(offset);
        instructions.push_back(wf::scene::render_instruction_t{
                    .instance = this,
                    .target   = transformed_target,
                    .damage   = damage,
                });
        for (auto & instance : instance_manager->get_instances())
        {
            instance->schedule_instructions(instructions, transformed_target, damage);
        }
    }
};


class simple_node_t : public wf::scene::node_t
{
  public:
    live_preview *preview;
    simple_node_t(live_preview *preview) : wf::scene::node_t(false)
    {
        LOGI("simple_node_t");
        this->preview = preview;
    }

    ~simple_node_t()
    {
        LOGI("~simple_node_t");
    }

    void gen_render_instances(std::vector<wf::scene::render_instance_uptr>& instances,
        wf::scene::damage_callback push_dmg, wf::output_t *shown_on) override
    {
        instances.push_back(std::make_unique<simple_node_render_instance_t>(
            this, push_dmg, preview));
    }

    void do_push_damage(wf::region_t updated_region)
    {
        wf::scene::node_damage_signal ev;
        ev.region = updated_region;
        this->emit(&ev);
    }
};

std::shared_ptr<simple_node_t> add_simple_node(live_preview *preview)
{
    LOGI("add_simple_node");
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
    wlr_backend *headless_backend = NULL;

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

            LOGI("request_stream");
            auto vg = toplevel->get_geometry();
            if (vg.width > vg.height)
            {
                auto scale = vg.height / double(vg.width);
                preview.down_scale = max_dimension / double(vg.width);
                vg.width  = max_dimension;
                vg.height = max_dimension * scale;
            } else
            {
                auto scale = vg.width / double(vg.height);
                preview.down_scale = max_dimension / double(vg.height);
                vg.height = max_dimension;
                vg.width  = max_dimension * scale;
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

                    preview.output->handle->scale = preview.down_scale;
                }
            }

            if (preview.output)
            {
                if (live_preview_render_node)
                {
                    wf::scene::remove_child(live_preview_render_node);
                    live_preview_render_node.reset();
                    live_preview_render_node = nullptr;
                }

                preview.view = view;
                live_preview_render_node = add_simple_node(&preview);
                view->connect(&view_unmapped);
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
            preview.output->handle->scale = preview.down_scale;

            if (live_preview_render_node)
            {
                wf::scene::remove_child(live_preview_render_node);
                live_preview_render_node.reset();
                live_preview_render_node = nullptr;
            }

            preview.view = view;
            live_preview_render_node = add_simple_node(&preview);
            view->connect(&view_unmapped);
            view->get_output()->render->damage_whole();
            preview.output->render->damage_whole();
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
        LOGI("release_output");
        if (live_preview_render_node)
        {
            wf::scene::remove_child(live_preview_render_node);
            live_preview_render_node.reset();
            live_preview_render_node = nullptr;
        }

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

        preview.view = nullptr;
        view_unmapped.disconnect();

        if (wf::get_core().seat->get_active_output() == output)
        {
            wf::get_core().seat->focus_output(
                wf::get_core().output_layout->get_next_output(output));
        }

        if (live_preview_render_node)
        {
            wf::scene::remove_child(live_preview_render_node);
            live_preview_render_node.reset();
            live_preview_render_node = nullptr;
        }

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
