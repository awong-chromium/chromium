/**************************************************************************
 * 
 * Copyright 2007 Tungsten Graphics, Inc., Cedar Park, Texas.
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
 * IN NO EVENT SHALL TUNGSTEN GRAPHICS AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 * 
 **************************************************************************/

/*
 * This file implements the st_draw_vbo() function which is called from
 * Mesa's VBO module.  All point/line/triangle rendering is done through
 * this function whether the user called glBegin/End, glDrawArrays,
 * glDrawElements, glEvalMesh, or glCalList, etc.
 *
 * We basically convert the VBO's vertex attribute/array information into
 * Gallium vertex state, bind the vertex buffer objects and call
 * pipe->draw_elements(), pipe->draw_range_elements() or pipe->draw_arrays().
 *
 * Authors:
 *   Keith Whitwell <keith@tungstengraphics.com>
 */


#include "main/imports.h"
#include "main/image.h"
#include "main/macros.h"
#include "program/prog_uniform.h"

#include "vbo/vbo.h"

#include "st_context.h"
#include "st_atom.h"
#include "st_cb_bufferobjects.h"
#include "st_draw.h"
#include "st_program.h"

#include "pipe/p_context.h"
#include "pipe/p_defines.h"
#include "util/u_inlines.h"
#include "util/u_format.h"
#include "util/u_prim.h"
#include "util/u_draw_quad.h"
#include "draw/draw_context.h"
#include "cso_cache/cso_context.h"


static GLuint double_types[4] = {
   PIPE_FORMAT_R64_FLOAT,
   PIPE_FORMAT_R64G64_FLOAT,
   PIPE_FORMAT_R64G64B64_FLOAT,
   PIPE_FORMAT_R64G64B64A64_FLOAT
};

static GLuint float_types[4] = {
   PIPE_FORMAT_R32_FLOAT,
   PIPE_FORMAT_R32G32_FLOAT,
   PIPE_FORMAT_R32G32B32_FLOAT,
   PIPE_FORMAT_R32G32B32A32_FLOAT
};

static GLuint half_float_types[4] = {
   PIPE_FORMAT_R16_FLOAT,
   PIPE_FORMAT_R16G16_FLOAT,
   PIPE_FORMAT_R16G16B16_FLOAT,
   PIPE_FORMAT_R16G16B16A16_FLOAT
};

static GLuint uint_types_norm[4] = {
   PIPE_FORMAT_R32_UNORM,
   PIPE_FORMAT_R32G32_UNORM,
   PIPE_FORMAT_R32G32B32_UNORM,
   PIPE_FORMAT_R32G32B32A32_UNORM
};

static GLuint uint_types_scale[4] = {
   PIPE_FORMAT_R32_USCALED,
   PIPE_FORMAT_R32G32_USCALED,
   PIPE_FORMAT_R32G32B32_USCALED,
   PIPE_FORMAT_R32G32B32A32_USCALED
};

static GLuint int_types_norm[4] = {
   PIPE_FORMAT_R32_SNORM,
   PIPE_FORMAT_R32G32_SNORM,
   PIPE_FORMAT_R32G32B32_SNORM,
   PIPE_FORMAT_R32G32B32A32_SNORM
};

static GLuint int_types_scale[4] = {
   PIPE_FORMAT_R32_SSCALED,
   PIPE_FORMAT_R32G32_SSCALED,
   PIPE_FORMAT_R32G32B32_SSCALED,
   PIPE_FORMAT_R32G32B32A32_SSCALED
};

static GLuint ushort_types_norm[4] = {
   PIPE_FORMAT_R16_UNORM,
   PIPE_FORMAT_R16G16_UNORM,
   PIPE_FORMAT_R16G16B16_UNORM,
   PIPE_FORMAT_R16G16B16A16_UNORM
};

static GLuint ushort_types_scale[4] = {
   PIPE_FORMAT_R16_USCALED,
   PIPE_FORMAT_R16G16_USCALED,
   PIPE_FORMAT_R16G16B16_USCALED,
   PIPE_FORMAT_R16G16B16A16_USCALED
};

static GLuint short_types_norm[4] = {
   PIPE_FORMAT_R16_SNORM,
   PIPE_FORMAT_R16G16_SNORM,
   PIPE_FORMAT_R16G16B16_SNORM,
   PIPE_FORMAT_R16G16B16A16_SNORM
};

static GLuint short_types_scale[4] = {
   PIPE_FORMAT_R16_SSCALED,
   PIPE_FORMAT_R16G16_SSCALED,
   PIPE_FORMAT_R16G16B16_SSCALED,
   PIPE_FORMAT_R16G16B16A16_SSCALED
};

static GLuint ubyte_types_norm[4] = {
   PIPE_FORMAT_R8_UNORM,
   PIPE_FORMAT_R8G8_UNORM,
   PIPE_FORMAT_R8G8B8_UNORM,
   PIPE_FORMAT_R8G8B8A8_UNORM
};

static GLuint ubyte_types_scale[4] = {
   PIPE_FORMAT_R8_USCALED,
   PIPE_FORMAT_R8G8_USCALED,
   PIPE_FORMAT_R8G8B8_USCALED,
   PIPE_FORMAT_R8G8B8A8_USCALED
};

static GLuint byte_types_norm[4] = {
   PIPE_FORMAT_R8_SNORM,
   PIPE_FORMAT_R8G8_SNORM,
   PIPE_FORMAT_R8G8B8_SNORM,
   PIPE_FORMAT_R8G8B8A8_SNORM
};

static GLuint byte_types_scale[4] = {
   PIPE_FORMAT_R8_SSCALED,
   PIPE_FORMAT_R8G8_SSCALED,
   PIPE_FORMAT_R8G8B8_SSCALED,
   PIPE_FORMAT_R8G8B8A8_SSCALED
};

static GLuint fixed_types[4] = {
   PIPE_FORMAT_R32_FIXED,
   PIPE_FORMAT_R32G32_FIXED,
   PIPE_FORMAT_R32G32B32_FIXED,
   PIPE_FORMAT_R32G32B32A32_FIXED
};



/**
 * Return a PIPE_FORMAT_x for the given GL datatype and size.
 */
GLuint
st_pipe_vertex_format(GLenum type, GLuint size, GLenum format,
                      GLboolean normalized)
{
   assert((type >= GL_BYTE && type <= GL_DOUBLE) ||
          type == GL_FIXED || type == GL_HALF_FLOAT);
   assert(size >= 1);
   assert(size <= 4);
   assert(format == GL_RGBA || format == GL_BGRA);

   if (format == GL_BGRA) {
      /* this is an odd-ball case */
      assert(type == GL_UNSIGNED_BYTE);
      assert(normalized);
      return PIPE_FORMAT_B8G8R8A8_UNORM;
   }

   if (normalized) {
      switch (type) {
      case GL_DOUBLE: return double_types[size-1];
      case GL_FLOAT: return float_types[size-1];
      case GL_HALF_FLOAT: return half_float_types[size-1];
      case GL_INT: return int_types_norm[size-1];
      case GL_SHORT: return short_types_norm[size-1];
      case GL_BYTE: return byte_types_norm[size-1];
      case GL_UNSIGNED_INT: return uint_types_norm[size-1];
      case GL_UNSIGNED_SHORT: return ushort_types_norm[size-1];
      case GL_UNSIGNED_BYTE: return ubyte_types_norm[size-1];
      case GL_FIXED: return fixed_types[size-1];
      default: assert(0); return 0;
      }      
   }
   else {
      switch (type) {
      case GL_DOUBLE: return double_types[size-1];
      case GL_FLOAT: return float_types[size-1];
      case GL_HALF_FLOAT: return half_float_types[size-1];
      case GL_INT: return int_types_scale[size-1];
      case GL_SHORT: return short_types_scale[size-1];
      case GL_BYTE: return byte_types_scale[size-1];
      case GL_UNSIGNED_INT: return uint_types_scale[size-1];
      case GL_UNSIGNED_SHORT: return ushort_types_scale[size-1];
      case GL_UNSIGNED_BYTE: return ubyte_types_scale[size-1];
      case GL_FIXED: return fixed_types[size-1];
      default: assert(0); return 0;
      }      
   }
   return 0; /* silence compiler warning */
}





/**
 * Examine the active arrays to determine if we have interleaved
 * vertex arrays all living in one VBO, or all living in user space.
 * \param userSpace  returns whether the arrays are in user space.
 */
static GLboolean
is_interleaved_arrays(const struct st_vertex_program *vp,
                      const struct st_vp_varient *vpv,
                      const struct gl_client_array **arrays,
                      GLboolean *userSpace)
{
   GLuint attr;
   const struct gl_buffer_object *firstBufObj = NULL;
   GLint firstStride = -1;
   GLuint num_client_arrays = 0;
   const GLubyte *client_addr = NULL;

   for (attr = 0; attr < vpv->num_inputs; attr++) {
      const GLuint mesaAttr = vp->index_to_input[attr];
      const struct gl_buffer_object *bufObj = arrays[mesaAttr]->BufferObj;
      const GLsizei stride = arrays[mesaAttr]->StrideB; /* in bytes */

      if (firstStride < 0) {
         firstStride = stride;
      }
      else if (firstStride != stride) {
         return GL_FALSE;
      }
         
      if (!bufObj || !bufObj->Name) {
         num_client_arrays++;
         /* Try to detect if the client-space arrays are
          * "close" to each other.
          */
         if (!client_addr) {
            client_addr = arrays[mesaAttr]->Ptr;
         }
         else if (abs(arrays[mesaAttr]->Ptr - client_addr) > firstStride) {
            /* arrays start too far apart */
            return GL_FALSE;
         }
      }
      else if (!firstBufObj) {
         firstBufObj = bufObj;
      }
      else if (bufObj != firstBufObj) {
         return GL_FALSE;
      }
   }

   *userSpace = (num_client_arrays == vpv->num_inputs);
   /* debug_printf("user space: %s (%d arrays, %d inputs)\n",
      (int)*userSpace ? "Yes" : "No", num_client_arrays, vp->num_inputs); */

   return GL_TRUE;
}


/**
 * Compute the memory range occupied by the arrays.
 */
static void
get_arrays_bounds(const struct st_vertex_program *vp,
                  const struct st_vp_varient *vpv,
                  const struct gl_client_array **arrays,
                  GLuint max_index,
                  const GLubyte **low, const GLubyte **high)
{
   const GLubyte *low_addr = NULL;
   const GLubyte *high_addr = NULL;
   GLuint attr;

   /* debug_printf("get_arrays_bounds: Handling %u attrs\n", vpv->num_inputs); */

   for (attr = 0; attr < vpv->num_inputs; attr++) {
      const GLuint mesaAttr = vp->index_to_input[attr];
      const GLint stride = arrays[mesaAttr]->StrideB;
      const GLubyte *start = arrays[mesaAttr]->Ptr;
      const unsigned sz = (arrays[mesaAttr]->Size * 
                           _mesa_sizeof_type(arrays[mesaAttr]->Type));
      const GLubyte *end = start + (max_index * stride) + sz;

      /* debug_printf("attr %u: stride %d size %u start %p end %p\n",
         attr, stride, sz, start, end); */

      if (attr == 0) {
         low_addr = start;
         high_addr = end;
      }
      else {
         low_addr = MIN2(low_addr, start);
         high_addr = MAX2(high_addr, end);
      }
   }

   *low = low_addr;
   *high = high_addr;
}


/**
 * Set up for drawing interleaved arrays that all live in one VBO
 * or all live in user space.
 * \param vbuffer  returns vertex buffer info
 * \param velements  returns vertex element info
 */
static void
setup_interleaved_attribs(GLcontext *ctx,
                          const struct st_vertex_program *vp,
                          const struct st_vp_varient *vpv,
                          const struct gl_client_array **arrays,
                          GLuint max_index,
                          GLboolean userSpace,
                          struct pipe_vertex_buffer *vbuffer,
                          struct pipe_vertex_element velements[])
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = st->pipe;
   GLuint attr;
   const GLubyte *offset0 = NULL;

   for (attr = 0; attr < vpv->num_inputs; attr++) {
      const GLuint mesaAttr = vp->index_to_input[attr];
      struct gl_buffer_object *bufobj = arrays[mesaAttr]->BufferObj;
      struct st_buffer_object *stobj = st_buffer_object(bufobj);
      GLsizei stride = arrays[mesaAttr]->StrideB;

      /*printf("stobj %u = %p\n", attr, (void*)stobj);*/

      if (attr == 0) {
         const GLubyte *low, *high;

         get_arrays_bounds(vp, vpv, arrays, max_index, &low, &high);
         /* debug_printf("buffer range: %p %p range %d max index %u\n",
            low, high, high - low, max_index); */

         offset0 = low;
         if (userSpace) {
            vbuffer->buffer =
               pipe_user_buffer_create(pipe->screen, (void *) low, high - low,
				       PIPE_BIND_VERTEX_BUFFER);
            vbuffer->buffer_offset = 0;
         }
         else {
            vbuffer->buffer = NULL;
            pipe_resource_reference(&vbuffer->buffer, stobj->buffer);
            vbuffer->buffer_offset = pointer_to_offset(low);
         }
         vbuffer->stride = stride; /* in bytes */
         vbuffer->max_index = max_index;
      }

      velements[attr].src_offset =
         (unsigned) (arrays[mesaAttr]->Ptr - offset0);
      velements[attr].instance_divisor = 0;
      velements[attr].vertex_buffer_index = 0;
      velements[attr].src_format =
         st_pipe_vertex_format(arrays[mesaAttr]->Type,
                               arrays[mesaAttr]->Size,
                               arrays[mesaAttr]->Format,
                               arrays[mesaAttr]->Normalized);
      assert(velements[attr].src_format);
   }
}


/**
 * Set up a separate pipe_vertex_buffer and pipe_vertex_element for each
 * vertex attribute.
 * \param vbuffer  returns vertex buffer info
 * \param velements  returns vertex element info
 */
static void
setup_non_interleaved_attribs(GLcontext *ctx,
                              const struct st_vertex_program *vp,
                              const struct st_vp_varient *vpv,
                              const struct gl_client_array **arrays,
                              GLuint max_index,
                              GLboolean *userSpace,
                              struct pipe_vertex_buffer vbuffer[],
                              struct pipe_vertex_element velements[])
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = st->pipe;
   GLuint attr;

   for (attr = 0; attr < vpv->num_inputs; attr++) {
      const GLuint mesaAttr = vp->index_to_input[attr];
      struct gl_buffer_object *bufobj = arrays[mesaAttr]->BufferObj;
      GLsizei stride = arrays[mesaAttr]->StrideB;

      *userSpace = GL_FALSE;

      if (bufobj && bufobj->Name) {
         /* Attribute data is in a VBO.
          * Recall that for VBOs, the gl_client_array->Ptr field is
          * really an offset from the start of the VBO, not a pointer.
          */
         struct st_buffer_object *stobj = st_buffer_object(bufobj);
         assert(stobj->buffer);
         /*printf("stobj %u = %p\n", attr, (void*) stobj);*/

         vbuffer[attr].buffer = NULL;
         pipe_resource_reference(&vbuffer[attr].buffer, stobj->buffer);
         vbuffer[attr].buffer_offset = pointer_to_offset(arrays[mesaAttr]->Ptr);
         velements[attr].src_offset = 0;
      }
      else {
         /* attribute data is in user-space memory, not a VBO */
         uint bytes;
         /*printf("user-space array %d stride %d\n", attr, stride);*/
	
         *userSpace = GL_TRUE;

         /* wrap user data */
         if (arrays[mesaAttr]->Ptr) {
            /* user's vertex array */
            if (arrays[mesaAttr]->StrideB) {
               bytes = arrays[mesaAttr]->StrideB * (max_index + 1);
            }
            else {
               bytes = arrays[mesaAttr]->Size
                  * _mesa_sizeof_type(arrays[mesaAttr]->Type);
            }
            vbuffer[attr].buffer = 
	       pipe_user_buffer_create(pipe->screen,
				       (void *) arrays[mesaAttr]->Ptr, bytes,
				       PIPE_BIND_VERTEX_BUFFER);
         }
         else {
            /* no array, use ctx->Current.Attrib[] value */
            bytes = sizeof(ctx->Current.Attrib[0]);
            vbuffer[attr].buffer = 
	       pipe_user_buffer_create(pipe->screen,
				       (void *) ctx->Current.Attrib[mesaAttr],
				       bytes,
				       PIPE_BIND_VERTEX_BUFFER);
            stride = 0;
         }

         vbuffer[attr].buffer_offset = 0;
         velements[attr].src_offset = 0;
      }

      assert(velements[attr].src_offset <= 2048); /* 11-bit field */

      /* common-case setup */
      vbuffer[attr].stride = stride; /* in bytes */
      vbuffer[attr].max_index = max_index;
      velements[attr].instance_divisor = 0;
      velements[attr].vertex_buffer_index = attr;
      velements[attr].src_format
         = st_pipe_vertex_format(arrays[mesaAttr]->Type,
                                 arrays[mesaAttr]->Size,
                                 arrays[mesaAttr]->Format,
                                 arrays[mesaAttr]->Normalized);
      assert(velements[attr].src_format);
   }
}


static void
setup_index_buffer(GLcontext *ctx,
                   const struct _mesa_index_buffer *ib,
                   struct pipe_index_buffer *ibuffer)
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = st->pipe;

   memset(ibuffer, 0, sizeof(*ibuffer));
   if (ib) {
      struct gl_buffer_object *bufobj = ib->obj;

      switch (ib->type) {
      case GL_UNSIGNED_INT:
         ibuffer->index_size = 4;
         break;
      case GL_UNSIGNED_SHORT:
         ibuffer->index_size = 2;
         break;
      case GL_UNSIGNED_BYTE:
         ibuffer->index_size = 1;
         break;
      default:
         assert(0);
	 return;
      }

      /* get/create the index buffer object */
      if (bufobj && bufobj->Name) {
         /* elements/indexes are in a real VBO */
         struct st_buffer_object *stobj = st_buffer_object(bufobj);
         pipe_resource_reference(&ibuffer->buffer, stobj->buffer);
         ibuffer->offset = pointer_to_offset(ib->ptr);
      }
      else {
         /* element/indicies are in user space memory */
         ibuffer->buffer =
            pipe_user_buffer_create(pipe->screen, (void *) ib->ptr,
                                    ib->count * ibuffer->index_size,
                                    PIPE_BIND_INDEX_BUFFER);
      }
   }
}

/**
 * Prior to drawing, check that any uniforms referenced by the
 * current shader have been set.  If a uniform has not been set,
 * issue a warning.
 */
static void
check_uniforms(GLcontext *ctx)
{
   const struct gl_shader_program *shProg = ctx->Shader.CurrentProgram;
   if (shProg && shProg->LinkStatus) {
      GLuint i;
      for (i = 0; i < shProg->Uniforms->NumUniforms; i++) {
         const struct gl_uniform *u = &shProg->Uniforms->Uniforms[i];
         if (!u->Initialized) {
            _mesa_warning(ctx,
                          "Using shader with uninitialized uniform: %s",
                          u->Name);
         }
      }
   }
}


/**
 * Translate OpenGL primtive type (GL_POINTS, GL_TRIANGLE_STRIP, etc) to
 * the corresponding Gallium type.
 */
static unsigned
translate_prim(const GLcontext *ctx, unsigned prim)
{
   /* GL prims should match Gallium prims, spot-check a few */
   assert(GL_POINTS == PIPE_PRIM_POINTS);
   assert(GL_QUADS == PIPE_PRIM_QUADS);
   assert(GL_TRIANGLE_STRIP_ADJACENCY == PIPE_PRIM_TRIANGLE_STRIP_ADJACENCY);

   /* Avoid quadstrips if it's easy to do so:
    * Note: it's imporant to do the correct trimming if we change the prim type!
    * We do that wherever this function is called.
    */
   if (prim == GL_QUAD_STRIP &&
       ctx->Light.ShadeModel != GL_FLAT &&
       ctx->Polygon.FrontMode == GL_FILL &&
       ctx->Polygon.BackMode == GL_FILL)
      prim = GL_TRIANGLE_STRIP;

   return prim;
}



/**
 * This function gets plugged into the VBO module and is called when
 * we have something to render.
 * Basically, translate the information into the format expected by gallium.
 */
void
st_draw_vbo(GLcontext *ctx,
            const struct gl_client_array **arrays,
            const struct _mesa_prim *prims,
            GLuint nr_prims,
            const struct _mesa_index_buffer *ib,
	    GLboolean index_bounds_valid,
            GLuint min_index,
            GLuint max_index)
{
   struct st_context *st = st_context(ctx);
   struct pipe_context *pipe = st->pipe;
   const struct st_vertex_program *vp;
   const struct st_vp_varient *vpv;
   struct pipe_vertex_buffer vbuffer[PIPE_MAX_SHADER_INPUTS];
   GLuint attr;
   struct pipe_vertex_element velements[PIPE_MAX_ATTRIBS];
   unsigned num_vbuffers, num_velements;
   struct pipe_index_buffer ibuffer;
   GLboolean userSpace = GL_FALSE;
   GLboolean vertDataEdgeFlags;
   struct pipe_draw_info info;
   unsigned i;

   /* Mesa core state should have been validated already */
   assert(ctx->NewState == 0x0);

   /* Gallium probably doesn't want this in some cases. */
   if (!index_bounds_valid)
      if (!vbo_all_varyings_in_vbos(arrays))
	 vbo_get_minmax_index(ctx, prims, ib, &min_index, &max_index);

   /* sanity check for pointer arithmetic below */
   assert(sizeof(arrays[0]->Ptr[0]) == 1);

   vertDataEdgeFlags = arrays[VERT_ATTRIB_EDGEFLAG]->BufferObj &&
                       arrays[VERT_ATTRIB_EDGEFLAG]->BufferObj->Name;
   if (vertDataEdgeFlags != st->vertdata_edgeflags) {
      st->vertdata_edgeflags = vertDataEdgeFlags;
      st->dirty.st |= ST_NEW_EDGEFLAGS_DATA;
   }

   st_validate_state(st);

   /* must get these after state validation! */
   vp = st->vp;
   vpv = st->vp_varient;

#if 0
   if (MESA_VERBOSE & VERBOSE_GLSL) {
      check_uniforms(ctx);
   }
#else
   (void) check_uniforms;
#endif

   memset(velements, 0, sizeof(struct pipe_vertex_element) * vpv->num_inputs);
   /*
    * Setup the vbuffer[] and velements[] arrays.
    */
   if (is_interleaved_arrays(vp, vpv, arrays, &userSpace)) {
      /*printf("Draw interleaved\n");*/
      setup_interleaved_attribs(ctx, vp, vpv, arrays, max_index, userSpace,
                                vbuffer, velements);
      num_vbuffers = 1;
      num_velements = vpv->num_inputs;
      if (num_velements == 0)
         num_vbuffers = 0;
   }
   else {
      /*printf("Draw non-interleaved\n");*/
      setup_non_interleaved_attribs(ctx, vp, vpv, arrays, max_index,
                                    &userSpace, vbuffer, velements);
      num_vbuffers = vpv->num_inputs;
      num_velements = vpv->num_inputs;
   }

#if 0
   {
      GLuint i;
      for (i = 0; i < num_vbuffers; i++) {
         printf("buffers[%d].stride = %u\n", i, vbuffer[i].stride);
         printf("buffers[%d].max_index = %u\n", i, vbuffer[i].max_index);
         printf("buffers[%d].buffer_offset = %u\n", i, vbuffer[i].buffer_offset);
         printf("buffers[%d].buffer = %p\n", i, (void*) vbuffer[i].buffer);
      }
      for (i = 0; i < num_velements; i++) {
         printf("vlements[%d].vbuffer_index = %u\n", i, velements[i].vertex_buffer_index);
         printf("vlements[%d].src_offset = %u\n", i, velements[i].src_offset);
         printf("vlements[%d].format = %s\n", i, util_format_name(velements[i].src_format));
      }
   }
#endif

   pipe->set_vertex_buffers(pipe, num_vbuffers, vbuffer);
   cso_set_vertex_elements(st->cso_context, num_velements, velements);

   setup_index_buffer(ctx, ib, &ibuffer);
   pipe->set_index_buffer(pipe, &ibuffer);

   util_draw_init_info(&info);
   if (ib) {
      info.indexed = TRUE;
      if (min_index != ~0 && max_index != ~0) {
         info.min_index = min_index;
         info.max_index = max_index;
      }
   }

   /* do actual drawing */
   for (i = 0; i < nr_prims; i++) {
      info.mode = translate_prim( ctx, prims[i].mode );
      info.start = prims[i].start;
      info.count = prims[i].count;
      info.instance_count = prims[i].num_instances;
      info.index_bias = prims[i].basevertex;
      if (!ib) {
         info.min_index = info.start;
         info.max_index = info.start + info.count - 1;
      }

      if (u_trim_pipe_prim(info.mode, &info.count))
         pipe->draw_vbo(pipe, &info);
   }

   pipe_resource_reference(&ibuffer.buffer, NULL);

   /* unreference buffers (frees wrapped user-space buffer objects) */
   for (attr = 0; attr < num_vbuffers; attr++) {
      pipe_resource_reference(&vbuffer[attr].buffer, NULL);
      assert(!vbuffer[attr].buffer);
   }

   if (userSpace) 
   {
      pipe->set_vertex_buffers(pipe, 0, NULL);
   }
}


void st_init_draw( struct st_context *st )
{
   GLcontext *ctx = st->ctx;

   vbo_set_draw_func(ctx, st_draw_vbo);

#if FEATURE_feedback || FEATURE_rastpos
   st->draw = draw_create(st->pipe); /* for selection/feedback */

   /* Disable draw options that might convert points/lines to tris, etc.
    * as that would foul-up feedback/selection mode.
    */
   draw_wide_line_threshold(st->draw, 1000.0f);
   draw_wide_point_threshold(st->draw, 1000.0f);
   draw_enable_line_stipple(st->draw, FALSE);
   draw_enable_point_sprites(st->draw, FALSE);
#endif
}


void st_destroy_draw( struct st_context *st )
{
#if FEATURE_feedback || FEATURE_rastpos
   draw_destroy(st->draw);
#endif
}


