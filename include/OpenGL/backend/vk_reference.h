/* VKGL (c) 2018 Dominik Witczak
 *
 * This code is licensed under MIT license (see LICENSE.txt for details)
 */
#ifndef VKGL_VK_REFERENCE_H
#define VKGL_VK_REFERENCE_H

#include "Anvil/include/misc/types.h"
#include "GL/glcorearb.h"
#include "OpenGL/reference.h"
#include <chrono>

namespace OpenGL
{
    typedef struct VKBufferPayload
    {
        OpenGL::TimeMarker  backend_buffer_creation_time_marker;
        OpenGL::TimeMarker  backend_mem_block_creation_time_marker;
        Anvil::Buffer*      buffer_ptr;
        OpenGL::TimeMarker  frontend_object_creation_time_marker;
        GLuint              id;
        Anvil::MemoryBlock* memory_block_ptr;

        const VKBufferPayload(const GLuint&             in_id,
                              const OpenGL::TimeMarker& in_frontend_object_creation_time_marker,
                              Anvil::Buffer*            in_buffer_ptr,
                              const OpenGL::TimeMarker& in_backend_buffer_creation_time_marker,
                              Anvil::MemoryBlock*       in_memory_block_ptr,
                              const OpenGL::TimeMarker& in_backend_mem_block_creation_time_marker)
           :backend_buffer_creation_time_marker   (in_backend_buffer_creation_time_marker),
            backend_mem_block_creation_time_marker(in_backend_mem_block_creation_time_marker),
            buffer_ptr                            (in_buffer_ptr),
            frontend_object_creation_time_marker  (in_frontend_object_creation_time_marker),
            id                                    (in_id),
            memory_block_ptr                      (in_memory_block_ptr)
        {
            /* Stub */
        }

        bool operator==(const VKBufferPayload& in_ref) const
        {
            return (id                                     == in_ref.id                                     &&
                    backend_buffer_creation_time_marker    == in_ref.backend_buffer_creation_time_marker    &&
                    backend_mem_block_creation_time_marker == in_ref.backend_mem_block_creation_time_marker &&
                    frontend_object_creation_time_marker   == in_ref.frontend_object_creation_time_marker);
        }

        bool operator!=(const VKBufferPayload& in_ref) const
        {
            return (id                                     != in_ref.id                                     ||
                    backend_buffer_creation_time_marker    != in_ref.backend_buffer_creation_time_marker    ||
                    backend_mem_block_creation_time_marker != in_ref.backend_mem_block_creation_time_marker ||
                    frontend_object_creation_time_marker   != in_ref.frontend_object_creation_time_marker);
        }
    } VKBufferPayload;

    typedef ReferenceBase<VKBufferPayload> VKBufferReference;
}

#endif /* VKGL_VK_REFERENCE_H */