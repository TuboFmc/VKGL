/* VKGL (c) 2018 Dominik Witczak
 *
 * This code is licensed under MIT license (see LICENSE.txt for details)
 */
#include "OpenGL/backend/vk_frame_graph.h"
#include "OpenGL/backend/vk_scheduler.h"
#include "OpenGL/backend/nodes/vk_buffer_data_node.h"
#include "OpenGL/frontend/gl_buffer_manager.h"
#include "Common/logger.h"

#define N_MAX_SCHEDULED_COMMANDS_LOG_2 (16)
#define WAIT_PERIOD_MS                 (1000)

OpenGL::VKScheduler::VKScheduler(const IContextObjectManagers* in_frontend_ptr,
                                 IVKBufferManager*             in_backend_buffer_manager_ptr,
                                 OpenGL::VKFrameGraph*         in_backend_frame_graph_manager_ptr)
    :m_backend_buffer_manager_ptr     (in_backend_buffer_manager_ptr),
     m_backend_frame_graph_manager_ptr(in_backend_frame_graph_manager_ptr),
     m_frontend_ptr                   (in_frontend_ptr),
     m_terminating                    (false)
{
    vkgl_assert(in_backend_buffer_manager_ptr != nullptr);
    vkgl_assert(in_frontend_ptr               != nullptr);
}

OpenGL::VKScheduler::~VKScheduler()
{
    /* Set a terminate flag and wait for the scheduler thread to quit */
    if (m_scheduler_thread_ptr != nullptr)
    {
        m_terminating.exchange(true);

        m_scheduler_thread_ptr->join();
    }

    /* Only after the thread dies, can we release the ring buffer instance. */
    m_command_ring_buffer_ptr.reset();
}

OpenGL::VKSchedulerUniquePtr OpenGL::VKScheduler::create(const IContextObjectManagers* in_frontend_ptr,
                                                         IVKBufferManager*             in_backend_buffer_manager_ptr,
                                                         OpenGL::VKFrameGraph*         in_backend_frame_graph_manager_ptr)
{
    OpenGL::VKSchedulerUniquePtr result_ptr;

    result_ptr.reset(
        new OpenGL::VKScheduler(in_frontend_ptr,
                                in_backend_buffer_manager_ptr,
                                in_backend_frame_graph_manager_ptr)
    );

    if (result_ptr != nullptr)
    {
        if (!result_ptr->init() )
        {
            result_ptr.reset();
        }
    }

    return result_ptr;
}

bool OpenGL::VKScheduler::init()
{
    bool result = false;

    /* 1. Instantiate the ring buffer. */
    m_command_ring_buffer_ptr.reset(
        new VKGL::RingBuffer<CommandBaseUniquePtr>(N_MAX_SCHEDULED_COMMANDS_LOG_2)
    );

    if (m_command_ring_buffer_ptr == nullptr)
    {
        vkgl_assert(m_command_ring_buffer_ptr != nullptr);

        goto end;
    }

    /* 2. Spawn the scheduler's main thread.. */
    m_scheduler_thread_ptr.reset(
        new std::thread(
            std::bind(&OpenGL::VKScheduler::main_thread_entrypoint,
                      this)
        )
    );

    if (m_scheduler_thread_ptr == nullptr)
    {
        vkgl_assert(m_scheduler_thread_ptr != nullptr);

        goto end;
    }

    /* Done! */
    result = true;
end:
    return result;
}

void OpenGL::VKScheduler::main_thread_entrypoint()
{
    /* NOTE: This entrypoint lives in its own dedicated thread */
    VKGL::g_logger_ptr->log(VKGL::LogLevel::Info,
                            "VK scheduler thread started.");

    do
    {
        OpenGL::CommandBaseUniquePtr command_ptr;

        if (!m_command_ring_buffer_ptr->grab_with_timeout(std::chrono::milliseconds(WAIT_PERIOD_MS),
                                                         &command_ptr) )
        {
            /* Time-out occurred, no commands have been submitted throughout the duration of the wait period..
             *
             * Check if the scheduler is winding up. If so, close the thread. Otherwise, keep on trying to
             * obtain a new command to process.
             */
            if (m_terminating.load() )
            {
                break;
            }

            continue;
        }

        process_command(std::move(command_ptr) );
    }
    while (true);

    VKGL::g_logger_ptr->log(VKGL::LogLevel::Info,
                            "VK scheduler thread quitting now.");
}

void OpenGL::VKScheduler::process_buffer_data_command(OpenGL::CommandBaseUniquePtr in_command_ptr)
{
    const OpenGL::VKBufferPayload*     backend_buffer_props_ptr     = nullptr;
    OpenGL::VKBufferReferenceUniquePtr backend_buffer_reference_ptr;
    OpenGL::BufferDataCommand*         command_ptr                  = dynamic_cast<OpenGL::BufferDataCommand*>(in_command_ptr.get() );
    OpenGL::VKFrameGraphNodeUniquePtr  node_ptr;

    vkgl_assert(command_ptr                       != nullptr);
    vkgl_assert(command_ptr->buffer_reference_ptr != nullptr);

    const auto& frontend_buffer_creation_time = command_ptr->buffer_reference_ptr->get_payload().object_creation_time;
    const auto& frontend_buffer_id            = command_ptr->buffer_reference_ptr->get_payload().id;
    const auto& frontend_buffer_snapshot_time = command_ptr->buffer_reference_ptr->get_payload().time_marker;

    /* 1. Retrieve backend buffer reference */
    {
        backend_buffer_reference_ptr = m_backend_buffer_manager_ptr->acquire_object(frontend_buffer_id,
                                                                                    frontend_buffer_creation_time,
                                                                                    m_backend_buffer_manager_ptr->get_tot_buffer_time_marker      (frontend_buffer_id,
                                                                                                                                                   frontend_buffer_creation_time),
                                                                                    m_backend_buffer_manager_ptr->get_tot_memory_block_time_marker(frontend_buffer_id,
                                                                                                                                                   frontend_buffer_creation_time) );

        vkgl_assert(backend_buffer_reference_ptr != nullptr);

        backend_buffer_props_ptr = &backend_buffer_reference_ptr->get_payload();
    }

    /* 2. Spawn the node */
    {
        OpenGL::VKFrameGraphNodeCreateInfoUniquePtr create_info_ptr(nullptr,
                                                                    std::default_delete<OpenGL::VKFrameGraphNodeCreateInfo>() );

        create_info_ptr.reset(new OpenGL::VKFrameGraphNodeCreateInfo() );
        vkgl_assert(create_info_ptr != nullptr);

        create_info_ptr->inputs.push_back(
            OpenGL::NodeIO(backend_buffer_reference_ptr->clone(),
                           0, /* in_start_offset */
                           command_ptr->size)

        );
        create_info_ptr->outputs.push_back(
            OpenGL::NodeIO(backend_buffer_reference_ptr->clone(),
                           0, /* in_start_offset */
                           command_ptr->size)

        );

        create_info_ptr->command_ptr = std::move(in_command_ptr);

        node_ptr = OpenGL::VKNodes::BufferData::create(std::move(create_info_ptr),
                                                       m_frontend_ptr,
                                                       m_backend_buffer_manager_ptr);
    }

    /* 3. Submit the node to frame graph manager. */
    m_backend_frame_graph_manager_ptr->add_node(std::move(node_ptr) );
}

void OpenGL::VKScheduler::process_buffer_sub_data_command(OpenGL::CommandBaseUniquePtr in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_clear_command(OpenGL::ClearCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_compile_shader_command(OpenGL::CompileShaderCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_command(OpenGL::CommandBaseUniquePtr in_command_ptr)
{
    switch (in_command_ptr->type)
    {
        case OpenGL::CommandType::BUFFER_DATA:                 process_buffer_data_command                (std::move(in_command_ptr) );                                                   break;
        case OpenGL::CommandType::BUFFER_SUB_DATA:             process_buffer_sub_data_command            (std::move(in_command_ptr) );                                                   break;
        case OpenGL::CommandType::CLEAR:                       process_clear_command                      (dynamic_cast<OpenGL::ClearCommand*>                  (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COMPILE_SHADER:              process_compile_shader_command             (dynamic_cast<OpenGL::CompileShaderCommand*>          (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COMPRESSED_TEX_IMAGE_1D:     process_compressed_tex_image_1D_command    (dynamic_cast<OpenGL::CompressedTexImage1DCommand*>   (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COMPRESSED_TEX_IMAGE_2D:     process_compressed_tex_image_2D_command    (dynamic_cast<OpenGL::CompressedTexImage2DCommand*>   (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COMPRESSED_TEX_IMAGE_3D:     process_compressed_tex_image_3D_command    (dynamic_cast<OpenGL::CompressedTexImage3DCommand*>   (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COMPRESSED_TEX_SUB_IMAGE_1D: process_compressed_tex_sub_image_1D_command(dynamic_cast<OpenGL::CompressedTexSubImage1DCommand*>(in_command_ptr.get() )); break;
        case OpenGL::CommandType::COMPRESSED_TEX_SUB_IMAGE_2D: process_compressed_tex_sub_image_2D_command(dynamic_cast<OpenGL::CompressedTexSubImage2DCommand*>(in_command_ptr.get() )); break;
        case OpenGL::CommandType::COMPRESSED_TEX_SUB_IMAGE_3D: process_compressed_tex_sub_image_3D_command(dynamic_cast<OpenGL::CompressedTexSubImage3DCommand*>(in_command_ptr.get() )); break;
        case OpenGL::CommandType::COPY_BUFFER_SUB_DATA:        process_copy_buffer_sub_data_command       (dynamic_cast<OpenGL::CopyBufferSubDataCommand*>      (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COPY_TEX_IMAGE_1D:           process_copy_tex_image_1D_command          (dynamic_cast<OpenGL::CopyTexImage1DCommand*>         (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COPY_TEX_IMAGE_2D:           process_copy_tex_image_2D_command          (dynamic_cast<OpenGL::CopyTexImage2DCommand*>         (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COPY_TEX_SUB_IMAGE_1D:       process_copy_tex_sub_image_1D_command      (dynamic_cast<OpenGL::CopyTexSubImage1DCommand*>      (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COPY_TEX_SUB_IMAGE_2D:       process_copy_tex_sub_image_2D_command      (dynamic_cast<OpenGL::CopyTexSubImage2DCommand*>      (in_command_ptr.get() )); break;
        case OpenGL::CommandType::COPY_TEX_SUB_IMAGE_3D:       process_copy_tex_sub_image_3D_command      (dynamic_cast<OpenGL::CopyTexSubImage3DCommand*>      (in_command_ptr.get() )); break;
        case OpenGL::CommandType::DRAW_ARRAYS:                 process_draw_arrays_command                (dynamic_cast<OpenGL::DrawArraysCommand*>             (in_command_ptr.get() )); break;
        case OpenGL::CommandType::DRAW_ELEMENTS:               process_draw_elements_command              (dynamic_cast<OpenGL::DrawElementsCommand*>           (in_command_ptr.get() )); break;
        case OpenGL::CommandType::DRAW_RANGE_ELEMENTS:         process_draw_range_elements_command        (dynamic_cast<OpenGL::DrawRangeElementsCommand*>      (in_command_ptr.get() )); break;
        case OpenGL::CommandType::FINISH:                      process_finish_command                     (dynamic_cast<OpenGL::FinishCommand*>                 (in_command_ptr.get() )); break;
        case OpenGL::CommandType::FLUSH:                       process_flush_command                      (dynamic_cast<OpenGL::FlushCommand*>                  (in_command_ptr.get() )); break;
        case OpenGL::CommandType::FLUSH_MAPPED_BUFFER_RANGE:   process_flush_mapped_buffer_range_command  (dynamic_cast<OpenGL::FlushMappedBufferRangeCommand*> (in_command_ptr.get() )); break;
        case OpenGL::CommandType::GET_BUFFER_SUB_DATA:         process_get_buffer_sub_data_command        (dynamic_cast<OpenGL::GetBufferSubDataCommand*>       (in_command_ptr.get() )); break;
        case OpenGL::CommandType::GET_COMPRESSED_TEX_IMAGE:    process_get_compressed_tex_image_command   (dynamic_cast<OpenGL::GetCompressedTexImageCommand*>  (in_command_ptr.get() )); break;
        case OpenGL::CommandType::GET_TEXTURE_IMAGE:           process_get_texture_image_command          (dynamic_cast<OpenGL::GetTextureImageCommand*>        (in_command_ptr.get() )); break;
        case OpenGL::CommandType::LINK_PROGRAM:                process_link_program_command               (dynamic_cast<OpenGL::LinkProgramCommand*>            (in_command_ptr.get() )); break;
        case OpenGL::CommandType::MAP_BUFFER:                  process_map_buffer_command                 (dynamic_cast<OpenGL::MapBufferCommand*>              (in_command_ptr.get() )); break;
        case OpenGL::CommandType::MULTI_DRAW_ARRAYS:           process_multi_draw_arrays_command          (dynamic_cast<OpenGL::MultiDrawArraysCommand*>        (in_command_ptr.get() )); break;
        case OpenGL::CommandType::MULTI_DRAW_ELEMENTS:         process_multi_draw_elements_command        (dynamic_cast<OpenGL::MultiDrawElementsCommand*>      (in_command_ptr.get() )); break;
        case OpenGL::CommandType::READ_PIXELS:                 process_read_pixels_command                (dynamic_cast<OpenGL::ReadPixelsCommand*>             (in_command_ptr.get() )); break;
        case OpenGL::CommandType::TEX_IMAGE_1D:                process_tex_image_1D_command               (dynamic_cast<OpenGL::TexImage1DCommand*>             (in_command_ptr.get() )); break;
        case OpenGL::CommandType::TEX_IMAGE_2D:                process_tex_image_2D_command               (dynamic_cast<OpenGL::TexImage2DCommand*>             (in_command_ptr.get() )); break;
        case OpenGL::CommandType::TEX_IMAGE_3D:                process_tex_image_3D_command               (dynamic_cast<OpenGL::TexImage3DCommand*>             (in_command_ptr.get() )); break;
        case OpenGL::CommandType::TEX_SUB_IMAGE_1D:            process_tex_sub_image_1D_command           (dynamic_cast<OpenGL::TexSubImage1DCommand*>          (in_command_ptr.get() )); break;
        case OpenGL::CommandType::TEX_SUB_IMAGE_2D:            process_tex_sub_image_2D_command           (dynamic_cast<OpenGL::TexSubImage2DCommand*>          (in_command_ptr.get() )); break;
        case OpenGL::CommandType::TEX_SUB_IMAGE_3D:            process_tex_sub_image_3D_command           (dynamic_cast<OpenGL::TexSubImage3DCommand*>          (in_command_ptr.get() )); break;
        case OpenGL::CommandType::UNMAP_BUFFER:                process_unmap_buffer_command               (dynamic_cast<OpenGL::UnmapBufferCommand*>            (in_command_ptr.get() )); break;
        case OpenGL::CommandType::VALIDATE_PROGRAM:            process_validate_program_command           (dynamic_cast<OpenGL::ValidateProgramCommand*>        (in_command_ptr.get() )); break;

        default:
        {
            vkgl_assert_fail();
        }
    }

    /* NOTE: in_command_ptr's members may have been modified (eg. unique ptrs moved out elsewhere) at this point.
     *
     *       Assume the only thing that can be done about the ptr is to release underlying storage.
     */
}

void OpenGL::VKScheduler::process_compressed_tex_image_1D_command(OpenGL::CompressedTexImage1DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_compressed_tex_image_2D_command(OpenGL::CompressedTexImage2DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_compressed_tex_image_3D_command(OpenGL::CompressedTexImage3DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_compressed_tex_sub_image_1D_command(OpenGL::CompressedTexSubImage1DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_compressed_tex_sub_image_2D_command(OpenGL::CompressedTexSubImage2DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_compressed_tex_sub_image_3D_command(OpenGL::CompressedTexSubImage3DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_copy_buffer_sub_data_command(OpenGL::CopyBufferSubDataCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_copy_tex_image_1D_command(OpenGL::CopyTexImage1DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_copy_tex_image_2D_command(OpenGL::CopyTexImage2DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_copy_tex_sub_image_1D_command(OpenGL::CopyTexSubImage1DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_copy_tex_sub_image_2D_command(OpenGL::CopyTexSubImage2DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_copy_tex_sub_image_3D_command(OpenGL::CopyTexSubImage3DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_draw_arrays_command(OpenGL::DrawArraysCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_draw_elements_command(OpenGL::DrawElementsCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_draw_range_elements_command(OpenGL::DrawRangeElementsCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_finish_command(OpenGL::FinishCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_flush_command(OpenGL::FlushCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_flush_mapped_buffer_range_command(OpenGL::FlushMappedBufferRangeCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_get_buffer_sub_data_command(OpenGL::GetBufferSubDataCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_get_compressed_tex_image_command(OpenGL::GetCompressedTexImageCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_get_texture_image_command(OpenGL::GetTextureImageCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_link_program_command(OpenGL::LinkProgramCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_map_buffer_command(OpenGL::MapBufferCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_multi_draw_arrays_command(OpenGL::MultiDrawArraysCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_multi_draw_elements_command(OpenGL::MultiDrawElementsCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_read_pixels_command(OpenGL::ReadPixelsCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_tex_image_1D_command(OpenGL::TexImage1DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_tex_image_2D_command(OpenGL::TexImage2DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_tex_image_3D_command(OpenGL::TexImage3DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_tex_sub_image_1D_command(OpenGL::TexSubImage1DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_tex_sub_image_2D_command(OpenGL::TexSubImage2DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_tex_sub_image_3D_command(OpenGL::TexSubImage3DCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_unmap_buffer_command(OpenGL::UnmapBufferCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::process_validate_program_command(OpenGL::ValidateProgramCommand* in_command_ptr)
{
    vkgl_not_implemented();
}

void OpenGL::VKScheduler::submit(OpenGL::CommandBaseUniquePtr in_command_ptr)
{
    /* NOTE: This entrypoint is called from application thread. */
    vkgl_assert(m_command_ring_buffer_ptr != nullptr);

    m_command_ring_buffer_ptr->stash(std::move(in_command_ptr) );
}