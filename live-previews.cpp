/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2025 Scott Moreau
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
    wf::option_wrapper_t<int> max_dimension{"live-previews/max_dimension"};
    wayfire_view current_preview = nullptr;
    wf::output_t *wo = nullptr;
    wf::dimensions_t current_size;
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
        region += -wf::origin(wo->get_layout_geometry());
        region  =
            wo->render->get_target_framebuffer().framebuffer_region_from_geometry_region(region);
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
        instance_manager = std::make_unique<wf::scene::render_instance_manager_t>(nodes, push_damage, view->get_output());
        instance_manager->set_visibility_region(view->get_output()->get_layout_geometry());
    }

  public:

    void init() override
    {
        method_repository->register_method("live_previews/request_stream", request_stream);
        method_repository->register_method("live_previews/release_output", release_output);
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
                vg.width = vg.width * (max_dimension / float(vg.height));
                vg.height = max_dimension;
            }
            else
            {
                vg.height = vg.height * (max_dimension / float(vg.width));
                vg.width = max_dimension;
            }
            LOGI(vg.width, "x", vg.height);
            auto output_name = "live-preview-" + std::to_string(id);
            if (vg.width != current_size.width || vg.height != current_size.height)
            {
                destroy_output();
                current_size.width = vg.width;
                current_size.height = vg.height;
            }
            for (auto& output : wf::get_core().output_layout->get_outputs())
            {
                if (wlr_output_is_headless(output->handle) && std::string(output->handle->name) == output_name)
                {
                    return wf::ipc::json_ok();
                }
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
            wlr_output_state_finish(&state);
            auto global = handle->global;
            handle->global = NULL;
            wlr_output_set_name(handle, output_name.c_str());
            handle->global = global;
            wo = wf::get_core().output_layout->find_output(handle);
            wo->render->add_post(&post_hook);
            wo->render->add_effect(&damage_hook, wf::OUTPUT_EFFECT_PRE);
            view->connect(&view_unmapped);
            destroy_render_instance_manager();
            create_render_instance_manager(view);
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

        destroy_output();

        return wf::ipc::json_ok();
    };

    wf::effect_hook_t damage_hook = [=] ()
    {
        if (wo)
        {
            wo->render->damage_whole();
            wo->render->schedule_redraw();
        }
    };

    wf::post_hook_t post_hook = [=] (wf::auxilliary_buffer_t& src, const wf::render_buffer_t& dst)
    {
        if (!current_preview)
        {
            return;
        }

        wf::gles::run_in_context([&]
        {
            wf::auxilliary_buffer_t aux_buffer;
            current_preview->take_snapshot(aux_buffer);
            auto src_size = aux_buffer.get_size();
            auto dst_size = wo->get_relative_geometry();
            wf::gles::bind_render_buffer(dst);
            dst.blit(aux_buffer, wlr_fbox{0, 0, float(src_size.width), float(src_size.height)}, wf::geometry_t{0, 0, dst_size.width, dst_size.height});
            aux_buffer.free();
            current_preview->damage();
        });
    };

    wf::signal::connection_t<wf::view_unmapped_signal> view_unmapped = [=] (wf::view_unmapped_signal *ev)
    {
        if (ev->view != current_preview)
        {
            return;
        }

        view_unmapped.disconnect();

        destroy_output();

        destroy_render_instance_manager();
        current_preview = nullptr;
    };

   void destroy_output()
   {
       if (wo)
       {
           wo->render->rem_post(&post_hook);
           wo->render->rem_effect(&damage_hook);
           wlr_output_destroy(wo->handle);
           wo = NULL;
       }
   }

    void fini() override
    {
        method_repository->unregister_method("live_previews/request_stream");
        method_repository->unregister_method("live_previews/release_output");
        destroy_output();
        destroy_render_instance_manager();
        view_unmapped.disconnect();
    }
};
}
}

DECLARE_WAYFIRE_PLUGIN(wf::live_previews::live_previews_plugin);
