#ifndef VKGL_TYPES_H
#define VKGL_TYPES_H

#include "Khronos/GL/glcorearb.h"
#include <stdint.h>
#include <vector>

#if defined(_WIN32)
    #define VKGL_API __declspec(dllexport)
#else
    #define VKGL_API
#endif

#define VKGL_APIENTRY KHRONOS_APIENTRY

#if defined(_WIN32)
    #define VKGL_THREADLOCAL __declspec(thread)
#else
    #error todo
#endif

namespace VKGL
{
    /* Forward declarations */
    class Context;
}

#include "OpenGL/types_enums.h"
#include "OpenGL/types_macros.h"
#include "OpenGL/types_typedefs.h"
#include "OpenGL/types_structs.h"

#endif /* VKGL_TYPES_H */