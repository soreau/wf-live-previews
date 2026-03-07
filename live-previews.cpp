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
class live_previews_plugin : public wf::plugin_interface_t
{
    wf::option_wrapper_t<bool> destroy_output_after_timeout{"live-previews/destroy_output"};
    wf::option_wrapper_t<int> max_dimension{"live-previews/max_dimension"};
    wf::option_wrapper_t<int> frame_skip{"live-previews/frame_skip"};
    wf::wl_listener_wrapper on_session_active;
    wf::wl_timer<false> output_destroy_timer;
    wayfire_view current_preview = nullptr;
    wf::dimensions_t current_size;
    int output_destroy_timeout_ms;
    wf::output_t *wo = nullptr;
    bool hook_set    = false;
    double current_scale;
    int drop_frame;

  private:
    wf::shared_data::ref_ptr_t<wf::ipc::method_repository_t> method_repository;
    std::unique_ptr<wf::scene::render_instance_manager_t> instance_manager = nullptr;
    wlr_backend *headless_backend = NULL;

    scene::damage_callback push_damage = [=] (wf::region_t region)
    {
        if (!wo)
        {
            return;
        }

        // Damage is pushed up to the root in root coordinate system,
        // we need it in output-buffer-local coordinate system.
        region += -wf::origin(current_preview->get_bounding_box());
        wo->render->damage(region, true);
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

        std::vector<scene::node_ptr> nodes;
        nodes.push_back(view->get_root_node());
        instance_manager = std::make_unique<wf::scene::render_instance_manager_t>(nodes, push_damage,
            view->get_output());
        instance_manager->set_visibility_region(view->get_bounding_box());
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

        output_destroy_timeout_ms = 5000;
    }

    wf::ipc::method_callback request_stream = [=] (wf::json_t data)
    {
        auto id = wf::ipc::json_get_uint64(data, "id");
        if (auto view = wf::ipc::find_view_by_id(id))
        {
            auto vg = view->get_bounding_box();
            if (vg.width < vg.height)
            {
                current_scale = max_dimension / double(vg.height);
                vg.width  = vg.width * current_scale;
                vg.height = max_dimension;
            } else
            {
                current_scale = max_dimension / double(vg.width);
                vg.height     = vg.height * current_scale;
                vg.width = max_dimension;
            }

            auto size = wf::dimensions_t{vg.width, vg.height};

            drop_frame = int(frame_skip);

            if (size != current_size)
            {
                current_size.width  = size.width;
                current_size.height = size.height;
                if (wo)
                {
                    wlr_output_state state;
                    wlr_output_state_init(&state);
                    wlr_output_state_set_custom_mode(&state, size.width, size.height, 0);
                    if (wlr_output_test_state(wo->handle, &state))
                    {
                        wlr_output_commit_state(wo->handle, &state);
                    } else
                    {
                        destroy_output();
                    }
                }
            }

            if (wo)
            {
                if (!hook_set)
                {
                    wo->render->add_post(&post_hook);
                    hook_set = true;
                }

                view->connect(&view_unmapped);
                destroy_render_instance_manager();
                create_render_instance_manager(view);
                view->get_output()->render->damage_whole();
                wo->render->damage_whole();
                current_preview = view;
                view->damage();
                return wf::ipc::json_ok();
            }

            if (!headless_backend)
            {
                headless_backend = wlr_headless_backend_create(wf::get_core().ev_loop);
                wlr_multi_backend_add(wf::get_core().backend, headless_backend);
                wlr_backend_start(headless_backend);
            }

            auto handle = wlr_headless_add_output(headless_backend, size.width, size.height);
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
            wo = wf::get_core().output_layout->find_output(handle);
            if (!hook_set)
            {
                wo->render->add_post(&post_hook);
                hook_set = true;
            }

            view->connect(&view_unmapped);
            destroy_render_instance_manager();
            create_render_instance_manager(view);
            view->get_output()->render->damage_whole();
            wo->render->damage_whole();
            current_preview = view;
            view->damage();

            return wf::ipc::json_ok();
        }

        return wf::ipc::json_error("no such view");
    };

    wf::ipc::method_callback release_output = [=] (wf::json_t data)
    {
        destroy_render_instance_manager();
        view_unmapped.disconnect();

        if (hook_set)
        {
            wo->render->rem_post(&post_hook);
            hook_set = false;
        }

        output_destroy_timer.disconnect();
        if (destroy_output_after_timeout)
        {
            output_destroy_timer.set_timeout(output_destroy_timeout_ms, [=] ()
            {
                destroy_output();
            });
        }

        return wf::ipc::json_ok();
    };

    void take_snapshot(wf::render_target_t *target)
    {
        auto root_node = current_preview->get_surface_root_node();
        const wf::geometry_t bbox = root_node->get_bounding_box();

        current_scale = (bbox.width < bbox.height) ?
            (max_dimension / double(bbox.height)) :
            (max_dimension / double(bbox.width));

        target->geometry = bbox;
        target->scale    = current_scale;

        std::vector<scene::render_instance_uptr> instances;
        root_node->gen_render_instances(instances, [] (auto) {}, current_preview->get_output());

        render_pass_params_t params;
        params.background_color = {0, 0, 0, 0};
        params.damage    = bbox;
        params.target    = *target;
        params.instances = &instances;
        params.flags     = RPASS_CLEAR_BACKGROUND;
        render_pass_t::run(params);
    }

    wf::post_hook_t post_hook = [=] (wf::auxilliary_buffer_t& src, const wf::render_buffer_t& dst)
    {
        if (drop_frame++ >= int(frame_skip))
        {
            drop_frame = 0;
        } else
        {
            return;
        }

        if (!current_preview)
        {
            return;
        }

        wf::render_target_t target = wf::render_target_t(dst);
        this->take_snapshot(&target);

        output_destroy_timer.disconnect();
        if (destroy_output_after_timeout)
        {
            output_destroy_timer.set_timeout(output_destroy_timeout_ms, [=] ()
            {
                destroy_output();
            });
        }
    };

    wf::signal::connection_t<wf::view_unmapped_signal> view_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        if (!current_preview || (ev->view != current_preview))
        {
            return;
        }

        view_unmapped.disconnect();

        destroy_render_instance_manager();
        current_preview = nullptr;
        if (hook_set)
        {
            wo->render->rem_post(&post_hook);
            hook_set = false;
        }
    };

    void destroy_output()
    {
        auto output = wf::get_core().output_layout->find_output("live-preview");
        output_destroy_timer.disconnect();
        if (!output)
        {
            return;
        }

        destroy_render_instance_manager();
        current_preview = nullptr;
        view_unmapped.disconnect();
        if (hook_set)
        {
            output->render->rem_post(&post_hook);
            hook_set = false;
        }

        if (wf::get_core().seat->get_active_output() == output)
        {
            wf::get_core().seat->focus_output(
                wf::get_core().output_layout->get_next_output(output));
        }

        wlr_output_layout_remove(wf::get_core().output_layout->get_handle(), output->handle);
        wlr_output_destroy(output->handle);

        if (output == wo)
        {
            wo = nullptr;
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
