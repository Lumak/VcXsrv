/**************************************************************************
 *
 * Copyright 2003 VMware, Inc.
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <stdio.h>
#include "arrayobj.h"
#include "glheader.h"
#include "c99_alloca.h"
#include "context.h"
#include "state.h"
#include "draw.h"
#include "draw_validate.h"
#include "dispatch.h"
#include "varray.h"
#include "bufferobj.h"
#include "enums.h"
#include "macros.h"
#include "transformfeedback.h"

typedef struct {
   GLuint count;
   GLuint primCount;
   GLuint first;
   GLuint baseInstance;
} DrawArraysIndirectCommand;

typedef struct {
   GLuint count;
   GLuint primCount;
   GLuint firstIndex;
   GLint  baseVertex;
   GLuint baseInstance;
} DrawElementsIndirectCommand;


/**
 * Check that element 'j' of the array has reasonable data.
 * Map VBO if needed.
 * For debugging purposes; not normally used.
 */
static void
check_array_data(struct gl_context *ctx, struct gl_vertex_array_object *vao,
                 GLuint attrib, GLuint j)
{
   const struct gl_array_attributes *array = &vao->VertexAttrib[attrib];
   if (array->Enabled) {
      const struct gl_vertex_buffer_binding *binding =
         &vao->BufferBinding[array->BufferBindingIndex];
      struct gl_buffer_object *bo = binding->BufferObj;
      const void *data = array->Ptr;
      if (_mesa_is_bufferobj(bo)) {
         if (!bo->Mappings[MAP_INTERNAL].Pointer) {
            /* need to map now */
            bo->Mappings[MAP_INTERNAL].Pointer =
               ctx->Driver.MapBufferRange(ctx, 0, bo->Size,
                                          GL_MAP_READ_BIT, bo, MAP_INTERNAL);
         }
         data = ADD_POINTERS(_mesa_vertex_attrib_address(array, binding),
                             bo->Mappings[MAP_INTERNAL].Pointer);
      }
      switch (array->Type) {
      case GL_FLOAT:
         {
            GLfloat *f = (GLfloat *) ((GLubyte *) data + binding->Stride * j);
            GLint k;
            for (k = 0; k < array->Size; k++) {
               if (IS_INF_OR_NAN(f[k]) || f[k] >= 1.0e20F || f[k] <= -1.0e10F) {
                  printf("Bad array data:\n");
                  printf("  Element[%u].%u = %f\n", j, k, f[k]);
                  printf("  Array %u at %p\n", attrib, (void *) array);
                  printf("  Type 0x%x, Size %d, Stride %d\n",
                         array->Type, array->Size, binding->Stride);
                  printf("  Address/offset %p in Buffer Object %u\n",
                         array->Ptr, bo->Name);
                  f[k] = 1.0F;  /* XXX replace the bad value! */
               }
               /*assert(!IS_INF_OR_NAN(f[k])); */
            }
         }
         break;
      default:
         ;
      }
   }
}


/**
 * Unmap the buffer object referenced by given array, if mapped.
 */
static void
unmap_array_buffer(struct gl_context *ctx, struct gl_vertex_array_object *vao,
                   GLuint attrib)
{
   const struct gl_array_attributes *array = &vao->VertexAttrib[attrib];
   if (array->Enabled) {
      const struct gl_vertex_buffer_binding *binding =
         &vao->BufferBinding[array->BufferBindingIndex];
      struct gl_buffer_object *bo = binding->BufferObj;
      if (_mesa_is_bufferobj(bo) && _mesa_bufferobj_mapped(bo, MAP_INTERNAL)) {
         ctx->Driver.UnmapBuffer(ctx, bo, MAP_INTERNAL);
      }
   }
}


static inline int
sizeof_ib_type(GLenum type)
{
   switch (type) {
   case GL_UNSIGNED_INT:
      return sizeof(GLuint);
   case GL_UNSIGNED_SHORT:
      return sizeof(GLushort);
   case GL_UNSIGNED_BYTE:
      return sizeof(GLubyte);
   default:
      assert(!"unsupported index data type");
      /* In case assert is turned off */
      return 0;
   }
}

/**
 * Examine the array's data for NaNs, etc.
 * For debug purposes; not normally used.
 */
static void
check_draw_elements_data(struct gl_context *ctx, GLsizei count,
                         GLenum elemType, const void *elements,
                         GLint basevertex)
{
   struct gl_vertex_array_object *vao = ctx->Array.VAO;
   const void *elemMap;
   GLint i;
   GLuint k;

   if (_mesa_is_bufferobj(vao->IndexBufferObj)) {
      elemMap = ctx->Driver.MapBufferRange(ctx, 0,
                                           vao->IndexBufferObj->Size,
                                           GL_MAP_READ_BIT,
                                           vao->IndexBufferObj, MAP_INTERNAL);
      elements = ADD_POINTERS(elements, elemMap);
   }

   for (i = 0; i < count; i++) {
      GLuint j;

      /* j = element[i] */
      switch (elemType) {
      case GL_UNSIGNED_BYTE:
         j = ((const GLubyte *) elements)[i];
         break;
      case GL_UNSIGNED_SHORT:
         j = ((const GLushort *) elements)[i];
         break;
      case GL_UNSIGNED_INT:
         j = ((const GLuint *) elements)[i];
         break;
      default:
         unreachable("Unexpected index buffer type");
      }

      /* check element j of each enabled array */
      for (k = 0; k < VERT_ATTRIB_MAX; k++) {
         check_array_data(ctx, vao, k, j);
      }
   }

   if (_mesa_is_bufferobj(vao->IndexBufferObj)) {
      ctx->Driver.UnmapBuffer(ctx, vao->IndexBufferObj, MAP_INTERNAL);
   }

   for (k = 0; k < VERT_ATTRIB_MAX; k++) {
      unmap_array_buffer(ctx, vao, k);
   }
}


/**
 * Check array data, looking for NaNs, etc.
 */
static void
check_draw_arrays_data(struct gl_context *ctx, GLint start, GLsizei count)
{
   /* TO DO */
}


/**
 * Check if we should skip the draw call even after validation was successful.
 */
static bool
skip_validated_draw(struct gl_context *ctx)
{
   switch (ctx->API) {
   case API_OPENGLES2:
      /* For ES2, we can draw if we have a vertex program/shader). */
      return ctx->VertexProgram._Current == NULL;

   case API_OPENGLES:
      /* For OpenGL ES, only draw if we have vertex positions
       */
      if (!ctx->Array.VAO->VertexAttrib[VERT_ATTRIB_POS].Enabled)
         return true;
      break;

   case API_OPENGL_CORE:
      /* Section 7.3 (Program Objects) of the OpenGL 4.5 Core Profile spec
       * says:
       *
       *     "If there is no active program for the vertex or fragment shader
       *     stages, the results of vertex and/or fragment processing will be
       *     undefined. However, this is not an error."
       *
       * The fragment shader is not tested here because other state (e.g.,
       * GL_RASTERIZER_DISCARD) affects whether or not we actually care.
       */
      return ctx->VertexProgram._Current == NULL;

   case API_OPENGL_COMPAT:
      if (ctx->VertexProgram._Current != NULL) {
         /* Draw regardless of whether or not we have any vertex arrays.
          * (Ex: could draw a point using a constant vertex pos)
          */
         return false;
      } else {
         /* Draw if we have vertex positions (GL_VERTEX_ARRAY or generic
          * array [0]).
          */
         return (!ctx->Array.VAO->VertexAttrib[VERT_ATTRIB_POS].Enabled &&
                 !ctx->Array.VAO->VertexAttrib[VERT_ATTRIB_GENERIC0].Enabled);
      }
      break;

   default:
      unreachable("Invalid API value in check_valid_to_render()");
   }

   return false;
}


/**
 * Print info/data for glDrawArrays(), for debugging.
 */
static void
print_draw_arrays(struct gl_context *ctx,
                  GLenum mode, GLint start, GLsizei count)
{
   const struct gl_vertex_array_object *vao = ctx->Array.VAO;

   printf("_mesa_DrawArrays(mode 0x%x, start %d, count %d):\n",
          mode, start, count);

   unsigned i;
   for (i = 0; i < VERT_ATTRIB_MAX; ++i) {
      const struct gl_array_attributes *array = &vao->VertexAttrib[i];
      if (!array->Enabled)
         continue;

      const struct gl_vertex_buffer_binding *binding =
         &vao->BufferBinding[array->BufferBindingIndex];
      struct gl_buffer_object *bufObj = binding->BufferObj;

      printf("attr %s: size %d stride %d  enabled %d  "
             "ptr %p  Bufobj %u\n",
             gl_vert_attrib_name((gl_vert_attrib) i),
             array->Size, binding->Stride, array->Enabled,
             array->Ptr, bufObj->Name);

      if (_mesa_is_bufferobj(bufObj)) {
         GLubyte *p = ctx->Driver.MapBufferRange(ctx, 0, bufObj->Size,
                                                 GL_MAP_READ_BIT, bufObj,
                                                 MAP_INTERNAL);
         int offset = (int) (GLintptr)
            _mesa_vertex_attrib_address(array, binding);

         unsigned multiplier;
         switch (array->Type) {
         case GL_DOUBLE:
         case GL_INT64_ARB:
         case GL_UNSIGNED_INT64_ARB:
            multiplier = 2;
            break;
         default:
            multiplier = 1;
         }

         float *f = (float *) (p + offset);
         int *k = (int *) f;
         int i = 0;
         int n = (count - 1) * (binding->Stride / (4 * multiplier))
           + array->Size;
         if (n > 32)
            n = 32;
         printf("  Data at offset %d:\n", offset);
         do {
            if (multiplier == 2)
               printf("    double[%d] = 0x%016llx %lf\n", i,
                      ((unsigned long long *) k)[i], ((double *) f)[i]);
            else
               printf("    float[%d] = 0x%08x %f\n", i, k[i], f[i]);
            i++;
         } while (i < n);
         ctx->Driver.UnmapBuffer(ctx, bufObj, MAP_INTERNAL);
      }
   }
}


/**
 * Return a filter mask for the net enabled vao arrays.
 * This is to mask out arrays that would otherwise supersed required current
 * values for the fixed function shaders for example.
 */
static GLbitfield
enabled_filter(const struct gl_context *ctx)
{
   switch (ctx->VertexProgram._VPMode) {
   case VP_MODE_FF:
      /* When no vertex program is active (or the vertex program is generated
       * from fixed-function state).  We put the material values into the
       * generic slots.  Since the vao has no material arrays, mute these
       * slots from the enabled arrays so that the current material values
       * are pulled instead of the vao arrays.
       */
      return VERT_BIT_FF_ALL;

   case VP_MODE_SHADER:
      /* There are no shaders in OpenGL ES 1.x, so this code path should be
       * impossible to reach.  The meta code is careful to not use shaders in
       * ES1.
       */
      assert(ctx->API != API_OPENGLES);

      /* Other parts of the code assume that inputs[VERT_ATTRIB_POS] through
       * inputs[VERT_ATTRIB_FF_MAX] will be non-NULL.  However, in OpenGL
       * ES 2.0+ or OpenGL core profile, none of these arrays should ever
       * be enabled.
       */
      if (ctx->API != API_OPENGL_COMPAT)
         return VERT_BIT_GENERIC_ALL;

      return VERT_BIT_ALL;

   default:
      assert(0);
      return 0;
   }
}


/**
 * Helper function called by the other DrawArrays() functions below.
 * This is where we handle primitive restart for drawing non-indexed
 * arrays.  If primitive restart is enabled, it typically means
 * splitting one DrawArrays() into two.
 */
static void
_mesa_draw_arrays(struct gl_context *ctx, GLenum mode, GLint start,
                  GLsizei count, GLuint numInstances, GLuint baseInstance,
                  GLuint drawID)
{
   struct _mesa_prim prim;

   if (skip_validated_draw(ctx))
      return;

   /* OpenGL 4.5 says that primitive restart is ignored with non-indexed
    * draws.
    */
   memset(&prim, 0, sizeof(prim));
   prim.begin = 1;
   prim.end = 1;
   prim.mode = mode;
   prim.num_instances = numInstances;
   prim.base_instance = baseInstance;
   prim.draw_id = drawID;
   prim.is_indirect = 0;
   prim.start = start;
   prim.count = count;

   ctx->Driver.Draw(ctx, &prim, 1, NULL,
                    GL_TRUE, start, start + count - 1, NULL, 0, NULL);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


/**
 * Execute a glRectf() function.
 */
static void GLAPIENTRY
_mesa_exec_Rectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
   GET_CURRENT_CONTEXT(ctx);
   ASSERT_OUTSIDE_BEGIN_END(ctx);

   CALL_Begin(GET_DISPATCH(), (GL_QUADS));
   CALL_Vertex2f(GET_DISPATCH(), (x1, y1));
   CALL_Vertex2f(GET_DISPATCH(), (x2, y1));
   CALL_Vertex2f(GET_DISPATCH(), (x2, y2));
   CALL_Vertex2f(GET_DISPATCH(), (x1, y2));
   CALL_End(GET_DISPATCH(), ());
}


static void GLAPIENTRY
_mesa_exec_EvalMesh1(GLenum mode, GLint i1, GLint i2)
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;
   GLfloat u, du;
   GLenum prim;

   switch (mode) {
   case GL_POINT:
      prim = GL_POINTS;
      break;
   case GL_LINE:
      prim = GL_LINE_STRIP;
      break;
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "glEvalMesh1(mode)");
      return;
   }

   /* No effect if vertex maps disabled.
    */
   if (!ctx->Eval.Map1Vertex4 && !ctx->Eval.Map1Vertex3)
      return;

   du = ctx->Eval.MapGrid1du;
   u = ctx->Eval.MapGrid1u1 + i1 * du;

   CALL_Begin(GET_DISPATCH(), (prim));
   for (i = i1; i <= i2; i++, u += du) {
      CALL_EvalCoord1f(GET_DISPATCH(), (u));
   }
   CALL_End(GET_DISPATCH(), ());
}


static void GLAPIENTRY
_mesa_exec_EvalMesh2(GLenum mode, GLint i1, GLint i2, GLint j1, GLint j2)
{
   GET_CURRENT_CONTEXT(ctx);
   GLfloat u, du, v, dv, v1, u1;
   GLint i, j;

   switch (mode) {
   case GL_POINT:
   case GL_LINE:
   case GL_FILL:
      break;
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "glEvalMesh2(mode)");
      return;
   }

   /* No effect if vertex maps disabled.
    */
   if (!ctx->Eval.Map2Vertex4 && !ctx->Eval.Map2Vertex3)
      return;

   du = ctx->Eval.MapGrid2du;
   dv = ctx->Eval.MapGrid2dv;
   v1 = ctx->Eval.MapGrid2v1 + j1 * dv;
   u1 = ctx->Eval.MapGrid2u1 + i1 * du;

   switch (mode) {
   case GL_POINT:
      CALL_Begin(GET_DISPATCH(), (GL_POINTS));
      for (v = v1, j = j1; j <= j2; j++, v += dv) {
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(GET_DISPATCH(), (u, v));
         }
      }
      CALL_End(GET_DISPATCH(), ());
      break;
   case GL_LINE:
      for (v = v1, j = j1; j <= j2; j++, v += dv) {
         CALL_Begin(GET_DISPATCH(), (GL_LINE_STRIP));
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(GET_DISPATCH(), (u, v));
         }
         CALL_End(GET_DISPATCH(), ());
      }
      for (u = u1, i = i1; i <= i2; i++, u += du) {
         CALL_Begin(GET_DISPATCH(), (GL_LINE_STRIP));
         for (v = v1, j = j1; j <= j2; j++, v += dv) {
            CALL_EvalCoord2f(GET_DISPATCH(), (u, v));
         }
         CALL_End(GET_DISPATCH(), ());
      }
      break;
   case GL_FILL:
      for (v = v1, j = j1; j < j2; j++, v += dv) {
         CALL_Begin(GET_DISPATCH(), (GL_TRIANGLE_STRIP));
         for (u = u1, i = i1; i <= i2; i++, u += du) {
            CALL_EvalCoord2f(GET_DISPATCH(), (u, v));
            CALL_EvalCoord2f(GET_DISPATCH(), (u, v + dv));
         }
         CALL_End(GET_DISPATCH(), ());
      }
      break;
   }
}


/**
 * Called from glDrawArrays when in immediate mode (not display list mode).
 */
void GLAPIENTRY
_mesa_DrawArrays(GLenum mode, GLint start, GLsizei count)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawArrays(%s, %d, %d)\n",
                  _mesa_enum_to_string(mode), start, count);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawArrays(ctx, mode, count))
         return;
   }

   if (0)
      check_draw_arrays_data(ctx, start, count);

   _mesa_draw_arrays(ctx, mode, start, count, 1, 0, 0);

   if (0)
      print_draw_arrays(ctx, mode, start, count);
}


/**
 * Called from glDrawArraysInstanced when in immediate mode (not
 * display list mode).
 */
void GLAPIENTRY
_mesa_DrawArraysInstanced(GLenum mode, GLint start, GLsizei count,
                          GLsizei numInstances)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawArraysInstanced(%s, %d, %d, %d)\n",
                  _mesa_enum_to_string(mode), start, count, numInstances);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawArraysInstanced(ctx, mode, start, count,
                                              numInstances))
         return;
   }

   if (0)
      check_draw_arrays_data(ctx, start, count);

   _mesa_draw_arrays(ctx, mode, start, count, numInstances, 0, 0);

   if (0)
      print_draw_arrays(ctx, mode, start, count);
}


/**
 * Called from glDrawArraysInstancedBaseInstance when in immediate mode.
 */
static void GLAPIENTRY
_mesa_exec_DrawArraysInstancedBaseInstance(GLenum mode, GLint first,
                                           GLsizei count, GLsizei numInstances,
                                           GLuint baseInstance)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx,
                  "glDrawArraysInstancedBaseInstance(%s, %d, %d, %d, %d)\n",
                  _mesa_enum_to_string(mode), first, count,
                  numInstances, baseInstance);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawArraysInstanced(ctx, mode, first, count,
                                              numInstances))
         return;
   }

   if (0)
      check_draw_arrays_data(ctx, first, count);

   _mesa_draw_arrays(ctx, mode, first, count, numInstances, baseInstance, 0);

   if (0)
      print_draw_arrays(ctx, mode, first, count);
}


/**
 * Called from glMultiDrawArrays when in immediate mode.
 */
static void GLAPIENTRY
_mesa_exec_MultiDrawArrays(GLenum mode, const GLint *first,
                           const GLsizei *count, GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx,
                  "glMultiDrawArrays(%s, %p, %p, %d)\n",
                  _mesa_enum_to_string(mode), first, count, primcount);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_MultiDrawArrays(ctx, mode, count, primcount))
         return;
   }

   for (i = 0; i < primcount; i++) {
      if (count[i] > 0) {
         if (0)
            check_draw_arrays_data(ctx, first[i], count[i]);

         /* The GL_ARB_shader_draw_parameters spec adds the following after the
          * pseudo-code describing glMultiDrawArrays:
          *
          *    "The index of the draw (<i> in the above pseudo-code) may be
          *     read by a vertex shader as <gl_DrawIDARB>, as described in
          *     Section 11.1.3.9."
          */
         _mesa_draw_arrays(ctx, mode, first[i], count[i], 1, 0, i);

         if (0)
            print_draw_arrays(ctx, mode, first[i], count[i]);
      }
   }
}



/**
 * Map GL_ELEMENT_ARRAY_BUFFER and print contents.
 * For debugging.
 */
#if 0
static void
dump_element_buffer(struct gl_context *ctx, GLenum type)
{
   const GLvoid *map =
      ctx->Driver.MapBufferRange(ctx, 0,
                                 ctx->Array.VAO->IndexBufferObj->Size,
                                 GL_MAP_READ_BIT,
                                 ctx->Array.VAO->IndexBufferObj,
                                 MAP_INTERNAL);
   switch (type) {
   case GL_UNSIGNED_BYTE:
      {
         const GLubyte *us = (const GLubyte *) map;
         GLint i;
         for (i = 0; i < ctx->Array.VAO->IndexBufferObj->Size; i++) {
            printf("%02x ", us[i]);
            if (i % 32 == 31)
               printf("\n");
         }
         printf("\n");
      }
      break;
   case GL_UNSIGNED_SHORT:
      {
         const GLushort *us = (const GLushort *) map;
         GLint i;
         for (i = 0; i < ctx->Array.VAO->IndexBufferObj->Size / 2; i++) {
            printf("%04x ", us[i]);
            if (i % 16 == 15)
               printf("\n");
         }
         printf("\n");
      }
      break;
   case GL_UNSIGNED_INT:
      {
         const GLuint *us = (const GLuint *) map;
         GLint i;
         for (i = 0; i < ctx->Array.VAO->IndexBufferObj->Size / 4; i++) {
            printf("%08x ", us[i]);
            if (i % 8 == 7)
               printf("\n");
         }
         printf("\n");
      }
      break;
   default:
      ;
   }

   ctx->Driver.UnmapBuffer(ctx, ctx->Array.VAO->IndexBufferObj, MAP_INTERNAL);
}
#endif


static bool
skip_draw_elements(struct gl_context *ctx, GLsizei count,
                   const GLvoid *indices)
{
   if (count == 0)
      return true;

   /* Not using a VBO for indices, so avoid NULL pointer derefs later.
    */
   if (!_mesa_is_bufferobj(ctx->Array.VAO->IndexBufferObj) && indices == NULL)
      return true;

   if (skip_validated_draw(ctx))
      return true;

   return false;
}


/**
 * Inner support for both _mesa_DrawElements and _mesa_DrawRangeElements.
 * Do the rendering for a glDrawElements or glDrawRangeElements call after
 * we've validated buffer bounds, etc.
 */
static void
_mesa_validated_drawrangeelements(struct gl_context *ctx, GLenum mode,
                                  GLboolean index_bounds_valid,
                                  GLuint start, GLuint end,
                                  GLsizei count, GLenum type,
                                  const GLvoid * indices,
                                  GLint basevertex, GLuint numInstances,
                                  GLuint baseInstance)
{
   struct _mesa_index_buffer ib;
   struct _mesa_prim prim;

   if (!index_bounds_valid) {
      assert(start == 0u);
      assert(end == ~0u);
   }

   if (skip_draw_elements(ctx, count, indices))
      return;

   ib.count = count;
   ib.index_size = sizeof_ib_type(type);
   ib.obj = ctx->Array.VAO->IndexBufferObj;
   ib.ptr = indices;

   prim.begin = 1;
   prim.end = 1;
   prim.pad = 0;
   prim.mode = mode;
   prim.start = 0;
   prim.count = count;
   prim.indexed = 1;
   prim.is_indirect = 0;
   prim.basevertex = basevertex;
   prim.num_instances = numInstances;
   prim.base_instance = baseInstance;
   prim.draw_id = 0;

   /* Need to give special consideration to rendering a range of
    * indices starting somewhere above zero.  Typically the
    * application is issuing multiple DrawRangeElements() to draw
    * successive primitives layed out linearly in the vertex arrays.
    * Unless the vertex arrays are all in a VBO (or locked as with
    * CVA), the OpenGL semantics imply that we need to re-read or
    * re-upload the vertex data on each draw call.
    *
    * In the case of hardware tnl, we want to avoid starting the
    * upload at zero, as it will mean every draw call uploads an
    * increasing amount of not-used vertex data.  Worse - in the
    * software tnl module, all those vertices might be transformed and
    * lit but never rendered.
    *
    * If we just upload or transform the vertices in start..end,
    * however, the indices will be incorrect.
    *
    * At this level, we don't know exactly what the requirements of
    * the backend are going to be, though it will likely boil down to
    * either:
    *
    * 1) Do nothing, everything is in a VBO and is processed once
    *       only.
    *
    * 2) Adjust the indices and vertex arrays so that start becomes
    *    zero.
    *
    * Rather than doing anything here, I'll provide a helper function
    * for the latter case elsewhere.
    */

   ctx->Driver.Draw(ctx, &prim, 1, &ib,
                    index_bounds_valid, start, end, NULL, 0, NULL);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


/**
 * Called by glDrawRangeElementsBaseVertex() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawRangeElementsBaseVertex(GLenum mode, GLuint start, GLuint end,
                                  GLsizei count, GLenum type,
                                  const GLvoid * indices, GLint basevertex)
{
   static GLuint warnCount = 0;
   GLboolean index_bounds_valid = GL_TRUE;

   /* This is only useful to catch invalid values in the "end" parameter
    * like ~0.
    */
   GLuint max_element = 2 * 1000 * 1000 * 1000; /* just a big number */

   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx,
                  "glDrawRangeElementsBaseVertex(%s, %u, %u, %d, %s, %p, %d)\n",
                  _mesa_enum_to_string(mode), start, end, count,
                  _mesa_enum_to_string(type), indices, basevertex);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawRangeElements(ctx, mode, start, end, count,
                                            type, indices))
         return;
   }

   if ((int) end + basevertex < 0 || start + basevertex >= max_element) {
      /* The application requested we draw using a range of indices that's
       * outside the bounds of the current VBO.  This is invalid and appears
       * to give undefined results.  The safest thing to do is to simply
       * ignore the range, in case the application botched their range tracking
       * but did provide valid indices.  Also issue a warning indicating that
       * the application is broken.
       */
      if (warnCount++ < 10) {
         _mesa_warning(ctx, "glDrawRangeElements(start %u, end %u, "
                       "basevertex %d, count %d, type 0x%x, indices=%p):\n"
                       "\trange is outside VBO bounds (max=%u); ignoring.\n"
                       "\tThis should be fixed in the application.",
                       start, end, basevertex, count, type, indices,
                       max_element - 1);
      }
      index_bounds_valid = GL_FALSE;
   }

   /* NOTE: It's important that 'end' is a reasonable value.
    * in _tnl_draw_prims(), we use end to determine how many vertices
    * to transform.  If it's too large, we can unnecessarily split prims
    * or we can read/write out of memory in several different places!
    */

   /* Catch/fix some potential user errors */
   if (type == GL_UNSIGNED_BYTE) {
      start = MIN2(start, 0xff);
      end = MIN2(end, 0xff);
   }
   else if (type == GL_UNSIGNED_SHORT) {
      start = MIN2(start, 0xffff);
      end = MIN2(end, 0xffff);
   }

   if (0) {
      printf("glDraw[Range]Elements{,BaseVertex}"
             "(start %u, end %u, type 0x%x, count %d) ElemBuf %u, "
             "base %d\n",
             start, end, type, count,
             ctx->Array.VAO->IndexBufferObj->Name, basevertex);
   }

   if ((int) start + basevertex < 0 || end + basevertex >= max_element)
      index_bounds_valid = GL_FALSE;

#if 0
   check_draw_elements_data(ctx, count, type, indices, basevertex);
#else
   (void) check_draw_elements_data;
#endif

   if (!index_bounds_valid) {
      start = 0;
      end = ~0;
   }

   _mesa_validated_drawrangeelements(ctx, mode, index_bounds_valid, start, end,
                                     count, type, indices, basevertex, 1, 0);
}


/**
 * Called by glDrawRangeElements() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawRangeElements(GLenum mode, GLuint start, GLuint end,
                        GLsizei count, GLenum type, const GLvoid * indices)
{
   if (MESA_VERBOSE & VERBOSE_DRAW) {
      GET_CURRENT_CONTEXT(ctx);
      _mesa_debug(ctx,
                  "glDrawRangeElements(%s, %u, %u, %d, %s, %p)\n",
                  _mesa_enum_to_string(mode), start, end, count,
                  _mesa_enum_to_string(type), indices);
   }

   _mesa_DrawRangeElementsBaseVertex(mode, start, end, count, type,
                                     indices, 0);
}


/**
 * Called by glDrawElements() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElements(GLenum mode, GLsizei count, GLenum type,
                   const GLvoid * indices)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawElements(%s, %u, %s, %p)\n",
                  _mesa_enum_to_string(mode), count,
                  _mesa_enum_to_string(type), indices);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawElements(ctx, mode, count, type, indices))
         return;
   }

   _mesa_validated_drawrangeelements(ctx, mode, GL_FALSE, 0, ~0,
                                     count, type, indices, 0, 1, 0);
}


/**
 * Called by glDrawElementsBaseVertex() in immediate mode.
 */
void GLAPIENTRY
_mesa_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                             const GLvoid * indices, GLint basevertex)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawElements(%s, %u, %s, %p)\n",
                  _mesa_enum_to_string(mode), count,
                  _mesa_enum_to_string(type), indices);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawElements(ctx, mode, count, type, indices))
         return;
   }

   _mesa_validated_drawrangeelements(ctx, mode, GL_FALSE, 0, ~0,
                                     count, type, indices, basevertex, 1, 0);
}


/**
 * Called by glDrawElementsInstanced() in immediate mode.
 */
static void GLAPIENTRY
_mesa_exec_DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type,
                                 const GLvoid * indices, GLsizei numInstances)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawElements(%s, %u, %s, %p)\n",
                  _mesa_enum_to_string(mode), count,
                  _mesa_enum_to_string(type), indices);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                                indices, numInstances))
         return;
   }

   _mesa_validated_drawrangeelements(ctx, mode, GL_FALSE, 0, ~0,
                                     count, type, indices, 0, numInstances, 0);
}


/**
 * Called by glDrawElementsInstancedBaseVertex() in immediate mode.
 */
static void GLAPIENTRY
_mesa_exec_DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count,
                                           GLenum type, const GLvoid * indices,
                                           GLsizei numInstances,
                                           GLint basevertex)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx,
                  "glDrawElementsInstancedBaseVertex"
                  "(%s, %d, %s, %p, %d; %d)\n",
                  _mesa_enum_to_string(mode), count,
                  _mesa_enum_to_string(type), indices,
                  numInstances, basevertex);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                                indices, numInstances))
         return;
   }

   _mesa_validated_drawrangeelements(ctx, mode, GL_FALSE, 0, ~0,
                                     count, type, indices,
                                     basevertex, numInstances, 0);
}


/**
 * Called by glDrawElementsInstancedBaseInstance() in immediate mode.
 */
static void GLAPIENTRY
_mesa_exec_DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count,
                                             GLenum type,
                                             const GLvoid *indices,
                                             GLsizei numInstances,
                                             GLuint baseInstance)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx,
                  "glDrawElementsInstancedBaseInstance"
                  "(%s, %d, %s, %p, %d, %d)\n",
                  _mesa_enum_to_string(mode), count,
                  _mesa_enum_to_string(type), indices,
                  numInstances, baseInstance);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                                indices, numInstances))
         return;
   }

   _mesa_validated_drawrangeelements(ctx, mode, GL_FALSE, 0, ~0,
                                     count, type, indices, 0, numInstances,
                                     baseInstance);
}


/**
 * Called by glDrawElementsInstancedBaseVertexBaseInstance() in immediate mode.
 */
static void GLAPIENTRY
_mesa_exec_DrawElementsInstancedBaseVertexBaseInstance(GLenum mode,
                                                       GLsizei count,
                                                       GLenum type,
                                                       const GLvoid *indices,
                                                       GLsizei numInstances,
                                                       GLint basevertex,
                                                       GLuint baseInstance)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx,
                  "glDrawElementsInstancedBaseVertexBaseInstance"
                  "(%s, %d, %s, %p, %d, %d, %d)\n",
                  _mesa_enum_to_string(mode), count,
                  _mesa_enum_to_string(type), indices,
                  numInstances, basevertex, baseInstance);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawElementsInstanced(ctx, mode, count, type,
                                                indices, numInstances))
         return;
   }

   _mesa_validated_drawrangeelements(ctx, mode, GL_FALSE, 0, ~0,
                                     count, type, indices, basevertex,
                                     numInstances, baseInstance);
}


/**
 * Inner support for both _mesa_MultiDrawElements() and
 * _mesa_MultiDrawRangeElements().
 * This does the actual rendering after we've checked array indexes, etc.
 */
static void
_mesa_validated_multidrawelements(struct gl_context *ctx, GLenum mode,
                                  const GLsizei *count, GLenum type,
                                  const GLvoid * const *indices,
                                  GLsizei primcount, const GLint *basevertex)
{
   struct _mesa_index_buffer ib;
   struct _mesa_prim *prim;
   unsigned int index_type_size = sizeof_ib_type(type);
   uintptr_t min_index_ptr, max_index_ptr;
   GLboolean fallback = GL_FALSE;
   int i;

   if (primcount == 0)
      return;

   prim = calloc(primcount, sizeof(*prim));
   if (prim == NULL) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "glMultiDrawElements");
      return;
   }

   min_index_ptr = (uintptr_t) indices[0];
   max_index_ptr = 0;
   for (i = 0; i < primcount; i++) {
      min_index_ptr = MIN2(min_index_ptr, (uintptr_t) indices[i]);
      max_index_ptr = MAX2(max_index_ptr, (uintptr_t) indices[i] +
                           index_type_size * count[i]);
   }

   /* Check if we can handle this thing as a bunch of index offsets from the
    * same index pointer.  If we can't, then we have to fall back to doing
    * a draw_prims per primitive.
    * Check that the difference between each prim's indexes is a multiple of
    * the index/element size.
    */
   if (index_type_size != 1) {
      for (i = 0; i < primcount; i++) {
         if ((((uintptr_t) indices[i] - min_index_ptr) % index_type_size) !=
             0) {
            fallback = GL_TRUE;
            break;
         }
      }
   }

   /* Draw primitives individually if one count is zero, so we can easily skip
    * that primitive.
    */
   for (i = 0; i < primcount; i++) {
      if (count[i] == 0) {
         fallback = GL_TRUE;
         break;
      }
   }

   /* If the index buffer isn't in a VBO, then treating the application's
    * subranges of the index buffer as one large index buffer may lead to
    * us reading unmapped memory.
    */
   if (!_mesa_is_bufferobj(ctx->Array.VAO->IndexBufferObj))
      fallback = GL_TRUE;

   if (!fallback) {
      ib.count = (max_index_ptr - min_index_ptr) / index_type_size;
      ib.index_size = sizeof_ib_type(type);
      ib.obj = ctx->Array.VAO->IndexBufferObj;
      ib.ptr = (void *) min_index_ptr;

      for (i = 0; i < primcount; i++) {
         prim[i].begin = (i == 0);
         prim[i].end = (i == primcount - 1);
         prim[i].pad = 0;
         prim[i].mode = mode;
         prim[i].start =
            ((uintptr_t) indices[i] - min_index_ptr) / index_type_size;
         prim[i].count = count[i];
         prim[i].indexed = 1;
         prim[i].num_instances = 1;
         prim[i].base_instance = 0;
         prim[i].draw_id = i;
         prim[i].is_indirect = 0;
         if (basevertex != NULL)
            prim[i].basevertex = basevertex[i];
         else
            prim[i].basevertex = 0;
      }

      ctx->Driver.Draw(ctx, prim, primcount, &ib,
                       false, 0, ~0, NULL, 0, NULL);
   }
   else {
      /* render one prim at a time */
      for (i = 0; i < primcount; i++) {
         if (count[i] == 0)
            continue;
         ib.count = count[i];
         ib.index_size = sizeof_ib_type(type);
         ib.obj = ctx->Array.VAO->IndexBufferObj;
         ib.ptr = indices[i];

         prim[0].begin = 1;
         prim[0].end = 1;
         prim[0].pad = 0;
         prim[0].mode = mode;
         prim[0].start = 0;
         prim[0].count = count[i];
         prim[0].indexed = 1;
         prim[0].num_instances = 1;
         prim[0].base_instance = 0;
         prim[0].draw_id = i;
         prim[0].is_indirect = 0;
         if (basevertex != NULL)
            prim[0].basevertex = basevertex[i];
         else
            prim[0].basevertex = 0;

         ctx->Driver.Draw(ctx, prim, 1, &ib, false, 0, ~0, NULL, 0, NULL);
      }
   }

   free(prim);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


void GLAPIENTRY
_mesa_MultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
                        const GLvoid * const *indices, GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (!_mesa_validate_MultiDrawElements(ctx, mode, count, type, indices,
                                         primcount))
      return;

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_multidrawelements(ctx, mode, count, type, indices, primcount,
                                     NULL);
}


void GLAPIENTRY
_mesa_MultiDrawElementsBaseVertex(GLenum mode,
                                  const GLsizei *count, GLenum type,
                                  const GLvoid * const *indices,
                                  GLsizei primcount,
                                  const GLsizei *basevertex)
{
   GET_CURRENT_CONTEXT(ctx);

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_MultiDrawElements(ctx, mode, count, type, indices,
                                            primcount))
         return;
   }

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_multidrawelements(ctx, mode, count, type, indices, primcount,
                                     basevertex);
}


/**
 * Draw a GL primitive using a vertex count obtained from transform feedback.
 * \param mode  the type of GL primitive to draw
 * \param obj  the transform feedback object to use
 * \param stream  index of the transform feedback stream from which to
 *                get the primitive count.
 * \param numInstances  number of instances to draw
 */
static void
_mesa_draw_transform_feedback(struct gl_context *ctx, GLenum mode,
                              struct gl_transform_feedback_object *obj,
                              GLuint stream, GLuint numInstances)
{
   struct _mesa_prim prim;

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawTransformFeedback(ctx, mode, obj, stream,
                                                numInstances)) {
         return;
      }
   }

   if (ctx->Driver.GetTransformFeedbackVertexCount &&
       (ctx->Const.AlwaysUseGetTransformFeedbackVertexCount ||
        !_mesa_all_varyings_in_vbos(ctx->Array.VAO))) {
      GLsizei n =
         ctx->Driver.GetTransformFeedbackVertexCount(ctx, obj, stream);
      _mesa_draw_arrays(ctx, mode, 0, n, numInstances, 0, 0);
      return;
   }

   if (skip_validated_draw(ctx))
      return;

   /* init most fields to zero */
   memset(&prim, 0, sizeof(prim));
   prim.begin = 1;
   prim.end = 1;
   prim.mode = mode;
   prim.num_instances = numInstances;
   prim.base_instance = 0;
   prim.is_indirect = 0;

   /* Maybe we should do some primitive splitting for primitive restart
    * (like in DrawArrays), but we have no way to know how many vertices
    * will be rendered. */

   ctx->Driver.Draw(ctx, &prim, 1, NULL, GL_FALSE, 0, ~0, obj, stream, NULL);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH) {
      _mesa_flush(ctx);
   }
}


/**
 * Like DrawArrays, but take the count from a transform feedback object.
 * \param mode  GL_POINTS, GL_LINES, GL_TRIANGLE_STRIP, etc.
 * \param name  the transform feedback object
 * User still has to setup of the vertex attribute info with
 * glVertexPointer, glColorPointer, etc.
 * Part of GL_ARB_transform_feedback2.
 */
void GLAPIENTRY
_mesa_DrawTransformFeedback(GLenum mode, GLuint name)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawTransformFeedback(%s, %d)\n",
                  _mesa_enum_to_string(mode), name);

   _mesa_draw_transform_feedback(ctx, mode, obj, 0, 1);
}


static void GLAPIENTRY
_mesa_exec_DrawTransformFeedbackStream(GLenum mode, GLuint name, GLuint stream)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawTransformFeedbackStream(%s, %u, %u)\n",
                  _mesa_enum_to_string(mode), name, stream);

   _mesa_draw_transform_feedback(ctx, mode, obj, stream, 1);
}


static void GLAPIENTRY
_mesa_exec_DrawTransformFeedbackInstanced(GLenum mode, GLuint name,
                                          GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawTransformFeedbackInstanced(%s, %d)\n",
                  _mesa_enum_to_string(mode), name);

   _mesa_draw_transform_feedback(ctx, mode, obj, 0, primcount);
}


static void GLAPIENTRY
_mesa_exec_DrawTransformFeedbackStreamInstanced(GLenum mode, GLuint name,
                                                GLuint stream,
                                                GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_transform_feedback_object *obj =
      _mesa_lookup_transform_feedback_object(ctx, name);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawTransformFeedbackStreamInstanced"
                  "(%s, %u, %u, %i)\n",
                  _mesa_enum_to_string(mode), name, stream, primcount);

   _mesa_draw_transform_feedback(ctx, mode, obj, stream, primcount);
}


static void
_mesa_validated_drawarraysindirect(struct gl_context *ctx,
                                   GLenum mode, const GLvoid *indirect)
{
   ctx->Driver.DrawIndirect(ctx, mode,
                            ctx->DrawIndirectBuffer, (GLsizeiptr) indirect,
                            1 /* draw_count */ , 16 /* stride */ ,
                            NULL, 0, NULL);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH)
      _mesa_flush(ctx);
}


static void
_mesa_validated_multidrawarraysindirect(struct gl_context *ctx,
                                        GLenum mode,
                                        const GLvoid *indirect,
                                        GLsizei primcount, GLsizei stride)
{
   GLsizeiptr offset = (GLsizeiptr) indirect;

   if (primcount == 0)
      return;

   ctx->Driver.DrawIndirect(ctx, mode, ctx->DrawIndirectBuffer, offset,
                            primcount, stride, NULL, 0, NULL);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH)
      _mesa_flush(ctx);
}


static void
_mesa_validated_drawelementsindirect(struct gl_context *ctx,
                                     GLenum mode, GLenum type,
                                     const GLvoid *indirect)
{
   struct _mesa_index_buffer ib;

   ib.count = 0;                /* unknown */
   ib.index_size = sizeof_ib_type(type);
   ib.obj = ctx->Array.VAO->IndexBufferObj;
   ib.ptr = NULL;

   ctx->Driver.DrawIndirect(ctx, mode,
                            ctx->DrawIndirectBuffer, (GLsizeiptr) indirect,
                            1 /* draw_count */ , 20 /* stride */ ,
                            NULL, 0, &ib);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH)
      _mesa_flush(ctx);
}


static void
_mesa_validated_multidrawelementsindirect(struct gl_context *ctx,
                                          GLenum mode, GLenum type,
                                          const GLvoid *indirect,
                                          GLsizei primcount, GLsizei stride)
{
   struct _mesa_index_buffer ib;
   GLsizeiptr offset = (GLsizeiptr) indirect;

   if (primcount == 0)
      return;

   /* NOTE: IndexBufferObj is guaranteed to be a VBO. */

   ib.count = 0;                /* unknown */
   ib.index_size = sizeof_ib_type(type);
   ib.obj = ctx->Array.VAO->IndexBufferObj;
   ib.ptr = NULL;

   ctx->Driver.DrawIndirect(ctx, mode,
                            ctx->DrawIndirectBuffer, offset,
                            primcount, stride, NULL, 0, &ib);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH)
      _mesa_flush(ctx);
}


/**
 * Like [Multi]DrawArrays/Elements, but they take most arguments from
 * a buffer object.
 */
static void GLAPIENTRY
_mesa_exec_DrawArraysIndirect(GLenum mode, const GLvoid *indirect)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawArraysIndirect(%s, %p)\n",
                  _mesa_enum_to_string(mode), indirect);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (ctx->API == API_OPENGL_COMPAT &&
       !_mesa_is_bufferobj(ctx->DrawIndirectBuffer)) {
      DrawArraysIndirectCommand *cmd = (DrawArraysIndirectCommand *) indirect;

      _mesa_exec_DrawArraysInstancedBaseInstance(mode, cmd->first, cmd->count,
                                                 cmd->primCount,
                                                 cmd->baseInstance);
      return;
   }

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawArraysIndirect(ctx, mode, indirect))
         return;
   }

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_drawarraysindirect(ctx, mode, indirect);
}


static void GLAPIENTRY
_mesa_exec_DrawElementsIndirect(GLenum mode, GLenum type, const GLvoid *indirect)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glDrawElementsIndirect(%s, %s, %p)\n",
                  _mesa_enum_to_string(mode),
                  _mesa_enum_to_string(type), indirect);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (ctx->API == API_OPENGL_COMPAT &&
       !_mesa_is_bufferobj(ctx->DrawIndirectBuffer)) {
      /*
       * Unlike regular DrawElementsInstancedBaseVertex commands, the indices
       * may not come from a client array and must come from an index buffer.
       * If no element array buffer is bound, an INVALID_OPERATION error is
       * generated.
       */
      if (!_mesa_is_bufferobj(ctx->Array.VAO->IndexBufferObj)) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "glDrawElementsIndirect(no buffer bound "
                     "to GL_ELEMENT_ARRAY_BUFFER)");
      } else {
         DrawElementsIndirectCommand *cmd =
            (DrawElementsIndirectCommand *) indirect;

         /* Convert offset to pointer */
         void *offset = (void *)(uintptr_t)
            ((cmd->firstIndex * _mesa_sizeof_type(type)) & 0xffffffffUL);

         _mesa_exec_DrawElementsInstancedBaseVertexBaseInstance(mode, cmd->count,
                                                                type, offset,
                                                                cmd->primCount,
                                                                cmd->baseVertex,
                                                                cmd->baseInstance);
      }

      return;
   }

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_DrawElementsIndirect(ctx, mode, type, indirect))
         return;
   }

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_drawelementsindirect(ctx, mode, type, indirect);
}


static void GLAPIENTRY
_mesa_exec_MultiDrawArraysIndirect(GLenum mode, const GLvoid *indirect,
                                   GLsizei primcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glMultiDrawArraysIndirect(%s, %p, %i, %i)\n",
                  _mesa_enum_to_string(mode), indirect, primcount, stride);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = sizeof(DrawArraysIndirectCommand);

   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (ctx->API == API_OPENGL_COMPAT &&
       !_mesa_is_bufferobj(ctx->DrawIndirectBuffer)) {

      if (!_mesa_valid_draw_indirect_multi(ctx, primcount, stride,
                                           "glMultiDrawArraysIndirect"))
         return;

      const ubyte *ptr = (const ubyte *) indirect;
      for (unsigned i = 0; i < primcount; i++) {
         DrawArraysIndirectCommand *cmd = (DrawArraysIndirectCommand *) ptr;
         _mesa_exec_DrawArraysInstancedBaseInstance(mode, cmd->first,
                                                    cmd->count, cmd->primCount,
                                                    cmd->baseInstance);

         if (stride == 0) {
            ptr += sizeof(DrawArraysIndirectCommand);
         } else {
            ptr += stride;
         }
      }

      return;
   }

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_MultiDrawArraysIndirect(ctx, mode, indirect,
                                                  primcount, stride))
         return;
   }

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_multidrawarraysindirect(ctx, mode, indirect,
                                           primcount, stride);
}


static void GLAPIENTRY
_mesa_exec_MultiDrawElementsIndirect(GLenum mode, GLenum type,
                                     const GLvoid *indirect,
                                     GLsizei primcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glMultiDrawElementsIndirect(%s, %s, %p, %i, %i)\n",
                  _mesa_enum_to_string(mode),
                  _mesa_enum_to_string(type), indirect, primcount, stride);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = sizeof(DrawElementsIndirectCommand);


   /* From the ARB_draw_indirect spec:
    *
    *    "Initially zero is bound to DRAW_INDIRECT_BUFFER. In the
    *    compatibility profile, this indicates that DrawArraysIndirect and
    *    DrawElementsIndirect are to source their arguments directly from the
    *    pointer passed as their <indirect> parameters."
    */
   if (ctx->API == API_OPENGL_COMPAT &&
       !_mesa_is_bufferobj(ctx->DrawIndirectBuffer)) {
      /*
       * Unlike regular DrawElementsInstancedBaseVertex commands, the indices
       * may not come from a client array and must come from an index buffer.
       * If no element array buffer is bound, an INVALID_OPERATION error is
       * generated.
       */
      if (!_mesa_is_bufferobj(ctx->Array.VAO->IndexBufferObj)) {
         _mesa_error(ctx, GL_INVALID_OPERATION,
                     "glMultiDrawElementsIndirect(no buffer bound "
                     "to GL_ELEMENT_ARRAY_BUFFER)");

         return;
      }

      if (!_mesa_valid_draw_indirect_multi(ctx, primcount, stride,
                                           "glMultiDrawArraysIndirect"))
         return;

      const ubyte *ptr = (const ubyte *) indirect;
      for (unsigned i = 0; i < primcount; i++) {
         _mesa_exec_DrawElementsIndirect(mode, type, ptr);

         if (stride == 0) {
            ptr += sizeof(DrawElementsIndirectCommand);
         } else {
            ptr += stride;
         }
      }

      return;
   }

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_MultiDrawElementsIndirect(ctx, mode, type, indirect,
                                                    primcount, stride))
         return;
   }

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_multidrawelementsindirect(ctx, mode, type, indirect,
                                             primcount, stride);
}


static void
_mesa_validated_multidrawarraysindirectcount(struct gl_context *ctx,
                                             GLenum mode,
                                             GLintptr indirect,
                                             GLintptr drawcount_offset,
                                             GLsizei maxdrawcount,
                                             GLsizei stride)
{
   GLsizeiptr offset = indirect;

   if (maxdrawcount == 0)
      return;

   ctx->Driver.DrawIndirect(ctx, mode,
                            ctx->DrawIndirectBuffer, offset,
                            maxdrawcount, stride,
                            ctx->ParameterBuffer, drawcount_offset, NULL);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH)
      _mesa_flush(ctx);
}


static void
_mesa_validated_multidrawelementsindirectcount(struct gl_context *ctx,
                                               GLenum mode, GLenum type,
                                               GLintptr indirect,
                                               GLintptr drawcount_offset,
                                               GLsizei maxdrawcount,
                                               GLsizei stride)
{
   struct _mesa_index_buffer ib;
   GLsizeiptr offset = (GLsizeiptr) indirect;

   if (maxdrawcount == 0)
      return;

   /* NOTE: IndexBufferObj is guaranteed to be a VBO. */

   ib.count = 0;                /* unknown */
   ib.index_size = sizeof_ib_type(type);
   ib.obj = ctx->Array.VAO->IndexBufferObj;
   ib.ptr = NULL;

   ctx->Driver.DrawIndirect(ctx, mode,
                            ctx->DrawIndirectBuffer, offset,
                            maxdrawcount, stride,
                            ctx->ParameterBuffer, drawcount_offset, &ib);

   if (MESA_DEBUG_FLAGS & DEBUG_ALWAYS_FLUSH)
      _mesa_flush(ctx);
}


static void GLAPIENTRY
_mesa_exec_MultiDrawArraysIndirectCount(GLenum mode, GLintptr indirect,
                                        GLintptr drawcount_offset,
                                        GLsizei maxdrawcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glMultiDrawArraysIndirectCountARB"
                  "(%s, %lx, %lx, %i, %i)\n",
                  _mesa_enum_to_string(mode),
                  (unsigned long) indirect, (unsigned long) drawcount_offset,
                  maxdrawcount, stride);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = 4 * sizeof(GLuint);      /* sizeof(DrawArraysIndirectCommand) */

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_MultiDrawArraysIndirectCount(ctx, mode,
                                                       indirect,
                                                       drawcount_offset,
                                                       maxdrawcount, stride))
         return;
   }

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_multidrawarraysindirectcount(ctx, mode, indirect,
                                                drawcount_offset,
                                                maxdrawcount, stride);
}


static void GLAPIENTRY
_mesa_exec_MultiDrawElementsIndirectCount(GLenum mode, GLenum type,
                                          GLintptr indirect,
                                          GLintptr drawcount_offset,
                                          GLsizei maxdrawcount, GLsizei stride)
{
   GET_CURRENT_CONTEXT(ctx);

   if (MESA_VERBOSE & VERBOSE_DRAW)
      _mesa_debug(ctx, "glMultiDrawElementsIndirectCountARB"
                  "(%s, %s, %lx, %lx, %i, %i)\n",
                  _mesa_enum_to_string(mode), _mesa_enum_to_string(type),
                  (unsigned long) indirect, (unsigned long) drawcount_offset,
                  maxdrawcount, stride);

   /* If <stride> is zero, the array elements are treated as tightly packed. */
   if (stride == 0)
      stride = 5 * sizeof(GLuint);      /* sizeof(DrawElementsIndirectCommand) */

   FLUSH_FOR_DRAW(ctx);

   _mesa_set_draw_vao(ctx, ctx->Array.VAO, enabled_filter(ctx));

   if (_mesa_is_no_error_enabled(ctx)) {
      if (ctx->NewState)
         _mesa_update_state(ctx);
   } else {
      if (!_mesa_validate_MultiDrawElementsIndirectCount(ctx, mode, type,
                                                         indirect,
                                                         drawcount_offset,
                                                         maxdrawcount, stride))
         return;
   }

   if (skip_validated_draw(ctx))
      return;

   _mesa_validated_multidrawelementsindirectcount(ctx, mode, type, indirect,
                                                  drawcount_offset, maxdrawcount,
                                                  stride);
}


/**
 * Initialize the dispatch table with the VBO functions for drawing.
 */
void
_mesa_initialize_exec_dispatch(const struct gl_context *ctx,
                               struct _glapi_table *exec)
{
   SET_DrawArrays(exec, _mesa_DrawArrays);
   SET_DrawElements(exec, _mesa_DrawElements);

   if (_mesa_is_desktop_gl(ctx) || _mesa_is_gles3(ctx)) {
      SET_DrawRangeElements(exec, _mesa_DrawRangeElements);
   }

   SET_MultiDrawArrays(exec, _mesa_exec_MultiDrawArrays);
   SET_MultiDrawElementsEXT(exec, _mesa_MultiDrawElements);

   if (ctx->API == API_OPENGL_COMPAT) {
      SET_Rectf(exec, _mesa_exec_Rectf);
      SET_EvalMesh1(exec, _mesa_exec_EvalMesh1);
      SET_EvalMesh2(exec, _mesa_exec_EvalMesh2);
   }

   if (ctx->API != API_OPENGLES &&
       ctx->Extensions.ARB_draw_elements_base_vertex) {
      SET_DrawElementsBaseVertex(exec, _mesa_DrawElementsBaseVertex);
      SET_MultiDrawElementsBaseVertex(exec,
                                      _mesa_MultiDrawElementsBaseVertex);

      if (_mesa_is_desktop_gl(ctx) || _mesa_is_gles3(ctx)) {
         SET_DrawRangeElementsBaseVertex(exec,
                                         _mesa_DrawRangeElementsBaseVertex);
         SET_DrawElementsInstancedBaseVertex(exec,
                                             _mesa_exec_DrawElementsInstancedBaseVertex);
      }
   }

   if (_mesa_is_desktop_gl(ctx) || _mesa_is_gles3(ctx)) {
      SET_DrawArraysInstancedBaseInstance(exec,
                                          _mesa_exec_DrawArraysInstancedBaseInstance);
      SET_DrawElementsInstancedBaseInstance(exec,
                                            _mesa_exec_DrawElementsInstancedBaseInstance);
      SET_DrawElementsInstancedBaseVertexBaseInstance(exec,
                                                      _mesa_exec_DrawElementsInstancedBaseVertexBaseInstance);
   }

   if (_mesa_is_desktop_gl(ctx) || _mesa_is_gles31(ctx)) {
      SET_DrawArraysIndirect(exec, _mesa_exec_DrawArraysIndirect);
      SET_DrawElementsIndirect(exec, _mesa_exec_DrawElementsIndirect);
   }

   if (_mesa_is_desktop_gl(ctx) || _mesa_is_gles3(ctx)) {
      SET_DrawArraysInstancedARB(exec, _mesa_DrawArraysInstanced);
      SET_DrawElementsInstancedARB(exec, _mesa_exec_DrawElementsInstanced);
   }

   if (_mesa_is_desktop_gl(ctx)) {
      SET_DrawTransformFeedback(exec, _mesa_DrawTransformFeedback);
      SET_DrawTransformFeedbackStream(exec,
                                      _mesa_exec_DrawTransformFeedbackStream);
      SET_DrawTransformFeedbackInstanced(exec,
                                         _mesa_exec_DrawTransformFeedbackInstanced);
      SET_DrawTransformFeedbackStreamInstanced(exec,
                                               _mesa_exec_DrawTransformFeedbackStreamInstanced);
      SET_MultiDrawArraysIndirect(exec, _mesa_exec_MultiDrawArraysIndirect);
      SET_MultiDrawElementsIndirect(exec, _mesa_exec_MultiDrawElementsIndirect);
      SET_MultiDrawArraysIndirectCountARB(exec,
                                          _mesa_exec_MultiDrawArraysIndirectCount);
      SET_MultiDrawElementsIndirectCountARB(exec,
                                            _mesa_exec_MultiDrawElementsIndirectCount);
   }
}



/* GL_IBM_multimode_draw_arrays */
void GLAPIENTRY
_mesa_MultiModeDrawArraysIBM( const GLenum * mode, const GLint * first,
                              const GLsizei * count,
                              GLsizei primcount, GLint modestride )
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;

   FLUSH_VERTICES(ctx, 0);

   for ( i = 0 ; i < primcount ; i++ ) {
      if ( count[i] > 0 ) {
         GLenum m = *((GLenum *) ((GLubyte *) mode + i * modestride));
         CALL_DrawArrays(ctx->CurrentServerDispatch, ( m, first[i], count[i] ));
      }
   }
}


/* GL_IBM_multimode_draw_arrays */
void GLAPIENTRY
_mesa_MultiModeDrawElementsIBM( const GLenum * mode, const GLsizei * count,
                                GLenum type, const GLvoid * const * indices,
                                GLsizei primcount, GLint modestride )
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;

   FLUSH_VERTICES(ctx, 0);

   /* XXX not sure about ARB_vertex_buffer_object handling here */

   for ( i = 0 ; i < primcount ; i++ ) {
      if ( count[i] > 0 ) {
         GLenum m = *((GLenum *) ((GLubyte *) mode + i * modestride));
         CALL_DrawElements(ctx->CurrentServerDispatch, ( m, count[i], type,
                                                         indices[i] ));
      }
   }
}


/*
 * Helper function for _mesa_draw_indirect below that additionally takes a zero
 * initialized array of _mesa_prim scratch space memory as the last argument.
 */
static void
draw_indirect(struct gl_context *ctx, GLuint mode,
              struct gl_buffer_object *indirect_data,
              GLsizeiptr indirect_offset, unsigned draw_count,
              unsigned stride,
              struct gl_buffer_object *indirect_draw_count_buffer,
              GLsizeiptr indirect_draw_count_offset,
              const struct _mesa_index_buffer *ib,
              struct _mesa_prim *prim)
{
   prim[0].begin = 1;
   prim[draw_count - 1].end = 1;
   for (unsigned i = 0; i < draw_count; ++i, indirect_offset += stride) {
      prim[i].mode = mode;
      prim[i].indexed = !!ib;
      prim[i].indirect_offset = indirect_offset;
      prim[i].is_indirect = 1;
      prim[i].draw_id = i;
   }

   /* This should always be true at this time */
   assert(indirect_data == ctx->DrawIndirectBuffer);

   ctx->Driver.Draw(ctx, prim, draw_count, ib, false, 0u, ~0u,
                    NULL, 0, indirect_data);
}


/*
 * Function to be put into dd_function_table::DrawIndirect as fallback.
 * Calls into dd_function_table::Draw past adapting call arguments.
 * See dd_function_table::DrawIndirect for call argument documentation.
 */
void
_mesa_draw_indirect(struct gl_context *ctx, GLuint mode,
                    struct gl_buffer_object *indirect_data,
                    GLsizeiptr indirect_offset, unsigned draw_count,
                    unsigned stride,
                    struct gl_buffer_object *indirect_draw_count_buffer,
                    GLsizeiptr indirect_draw_count_offset,
                    const struct _mesa_index_buffer *ib)
{
   /* Use alloca for the prim space if we are somehow in bounds. */
   if (draw_count*sizeof(struct _mesa_prim) < 1024) {
      struct _mesa_prim *space = alloca(draw_count*sizeof(struct _mesa_prim));
      memset(space, 0, draw_count*sizeof(struct _mesa_prim));

      draw_indirect(ctx, mode, indirect_data, indirect_offset, draw_count,
                    stride, indirect_draw_count_buffer,
                    indirect_draw_count_offset, ib, space);
   } else {
      struct _mesa_prim *space = calloc(draw_count, sizeof(struct _mesa_prim));
      if (space == NULL) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "gl%sDraw%sIndirect%s",
                     (draw_count > 1) ? "Multi" : "",
                     ib ? "Elements" : "Arrays",
                     indirect_data ? "CountARB" : "");
         return;
      }

      draw_indirect(ctx, mode, indirect_data, indirect_offset, draw_count,
                    stride, indirect_draw_count_buffer,
                    indirect_draw_count_offset, ib, space);

      free(space);
   }
}
