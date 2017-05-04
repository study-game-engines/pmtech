#include <stdlib.h>

#include "renderer.h"
#include "memory.h"
#include "pen_string.h"
#include "threads.h"
#include "timer.h"
#include "pen.h"
#include <vector>

//--------------------------------------------------------------------------------------
// Global Variables
//--------------------------------------------------------------------------------------

extern pen::window_creation_params pen_window;

extern void pen_make_gl_context_current( );
extern void pen_gl_swap_buffers( );

namespace pen
{
	//--------------------------------------------------------------------------------------
	//  COMMON API
	//--------------------------------------------------------------------------------------
	#define NUM_QUERY_BUFFERS		4
	#define MAX_QUERIES				64 
	#define NUM_CUBEMAP_FACES		6
    #define MAX_VERTEX_ATTRIBUTES   16
 
	#define QUERY_DISJOINT			1
	#define QUERY_ISSUED			(1<<1)
	#define QUERY_SO_STATS			(1<<2)
    
    struct context_state
	{
		context_state()
		{
			active_query_index = 0;
		}

		u32 backbuffer_colour;
		u32 backbuffer_depth;

		u32 active_colour_target;
		u32 active_depth_target;

		u32	active_query_index;

	};

    struct clear_state_internal
	{
		f32 rgba[ 4 ];
		f32 depth;
		u32 flags;
	};
    
    struct vertex_attribute
    {
        u32     location;
        u32     type;
        u32     stride;
        size_t  offset;
        u32     num_elements;
    };
    
    struct input_layout
    {
        std::vector<vertex_attribute> attributes;
        GLuint vertex_array_handle = 0;
        GLuint vb_handle = 0;
    };

    struct raster_state
    {
        GLenum cull_face;
        GLenum polygon_mode;
        bool culling_enabled;
        bool depth_clip_enabled;
        bool scissor_enabled;
    };
    
    struct texture_info
    {
        GLuint handle;
        u32 max_mip_level;
    };
    
    struct render_target
    {
        GLuint framebuffer;
        texture_info texture;
    };
    
    enum resource_type : s32
    {
        RES_TEXTURE = 0,
        RES_RENDER_TARGET
    };
    
    struct shader_program
    {
        u32 vs;
        u32 ps;
        u32 gs;
        GLuint program;
    };
    
    std::vector<shader_program> shader_programs;
    
	struct resource_allocation
	{
		u8      asigned_flag;
        GLuint  type;
        
		union 
		{
			clear_state_internal			clear_state;
            input_layout*                   input_layout;
            raster_state                    raster_state;
            depth_stencil_creation_params*  depth_stencil;
            blend_creation_params*          blend_state;
            GLuint                          handle;
            texture_info                    texture;
            render_target                   render_target;
            sampler_creation_params*        sampler_state;
            shader_program*                 shader_program;
		};
	};
    resource_allocation		 resource_pool	[MAX_RENDERER_RESOURCES];

    struct query_allocation
	{
		u8              asigned_flag;
		GLuint          query                  [NUM_QUERY_BUFFERS];
		u32             flags                  [NUM_QUERY_BUFFERS];
		a_u64           last_result;
	};
	query_allocation	query_pool		[MAX_QUERIES];
    
    struct active_state
    {
        u32 vertex_buffer;
        u32 vertex_buffer_stride;
        u32 index_buffer;
        u32 input_layout;
        u32 vertex_shader;
        u32 pixel_shader;
        u32 raster_state;
        bool enabled_vertex_attributes[MAX_VERTEX_ATTRIBUTES]; //todo remove
    };
    
    active_state g_bound_state;
    active_state g_current_state;

	void clear_resource_table( )
	{
		pen::memory_zero( &resource_pool[ 0 ], sizeof( resource_allocation ) * MAX_RENDERER_RESOURCES );
		
		//reserve resource 0 for NULL binding.
		resource_pool[0].asigned_flag |= 0xff;
	}

	void clear_query_table()
	{
		pen::memory_zero(&query_pool[0], sizeof(query_allocation) * MAX_QUERIES);
	}
    
	u32 get_next_query_index(u32 domain)
	{
		u32 i = 0;
		while (query_pool[i].asigned_flag & domain)
		{
			++i;
		}

		query_pool[i].asigned_flag |= domain;

		return i;
	};

	context_state			 g_context;

	u32 renderer_create_clear_state( const clear_state &cs )
	{
		u32 resoruce_index = renderer_get_next_resource_index( DIRECT_RESOURCE | DEFER_RESOURCE );

		resource_pool[ resoruce_index ].clear_state.rgba[ 0 ] = cs.r;
		resource_pool[ resoruce_index ].clear_state.rgba[ 1 ] = cs.g;
		resource_pool[ resoruce_index ].clear_state.rgba[ 2 ] = cs.b;
		resource_pool[ resoruce_index ].clear_state.rgba[ 3 ] = cs.a;
		resource_pool[ resoruce_index ].clear_state.depth = cs.depth;
		resource_pool[ resoruce_index ].clear_state.flags = cs.flags;

		return  resoruce_index;
	}

	f64 renderer_get_last_query(u32 query_index)
	{
		f64 res;
		pen::memory_cpy(&res, &query_pool[query_index].last_result, sizeof(f64));

		return res;
	}
    
    shader_program* link_program_internal( GLuint vs, GLuint ps )
    {
        //link the shaders
        GLuint program_id = glCreateProgram();
        
        glAttachShader(program_id, vs);
        glAttachShader(program_id, ps);
        glLinkProgram(program_id);
        
        // Check the program
        GLint result = GL_FALSE;
        int info_log_length;
        
        glGetShaderiv(program_id, GL_LINK_STATUS, &result);
        glGetShaderiv(program_id, GL_INFO_LOG_LENGTH, &info_log_length);
        
        if ( info_log_length > 0 )
        {
            char* info_log_buf = (char*)pen::memory_alloc(info_log_length + 1);
            
            glGetShaderInfoLog(program_id, info_log_length, NULL, &info_log_buf[0]);
            
            pen::string_output_debug(info_log_buf);
        }
        
        shader_program program;
        program.vs = g_bound_state.vertex_shader;
        program.ps = g_bound_state.pixel_shader;
        program.program = program_id;
        
        shader_programs.push_back(program);
        
        return &shader_programs.back();
    }

	//--------------------------------------------------------------------------------------
	//  DIRECT API
	//--------------------------------------------------------------------------------------    
    void direct::renderer_make_context_current( )
    {
        pen_make_gl_context_current();
    }

	void direct::renderer_clear( u32 clear_state_index, u32 colour_face, u32 depth_face )
	{
        resource_allocation& rc = resource_pool[ clear_state_index ];
        
        glClearColor( rc.clear_state.rgba[ 0 ], rc.clear_state.rgba[ 1 ], rc.clear_state.rgba[ 2 ], rc.clear_state.rgba[ 3 ] );
        glClearDepth( rc.clear_state.depth );
        glClear( rc.clear_state.flags );
	}

	void direct::renderer_present( )
	{
        pen_gl_swap_buffers();
	}

	void direct::renderer_create_query( u32 query_type, u32 flags )
	{
        //glGenQueries(1,1)
	}

	void direct::renderer_set_query(u32 query_index, u32 action)
	{

	}

	u32 direct::renderer_load_shader(const pen::shader_load_params &params)
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
        
        resource_allocation& res = resource_pool[ resource_index ];
        
        res.handle = glCreateShader(params.type);
        
        glShaderSource(res.handle, 1, (c8**)&params.byte_code, (s32*)&params.byte_code_size);
        glCompileShader(res.handle);
        
        // Check compilation status
        GLint result = GL_FALSE;
        int info_log_length;
        
        glGetShaderiv(res.handle, GL_COMPILE_STATUS, &result);
        glGetShaderiv(res.handle, GL_INFO_LOG_LENGTH, &info_log_length);
        
        if ( info_log_length > 0 )
        {
            char* info_log_buf = (char*)pen::memory_alloc(info_log_length + 1);
            
            glGetShaderInfoLog(res.handle, info_log_length, NULL, &info_log_buf[0]);
            
            pen::string_output_debug(info_log_buf);
        }

		return resource_index;
	}

	void direct::renderer_set_shader( u32 shader_index, u32 shader_type )
	{
        if( shader_type == GL_VERTEX_SHADER )
            g_current_state.vertex_shader = shader_index;
        else if( shader_type == GL_FRAGMENT_SHADER )
            g_current_state.pixel_shader = shader_index;
	}

	u32 direct::renderer_create_buffer( const buffer_creation_params &params )
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
        
        resource_allocation& res = resource_pool[resource_index];
        
        glGenBuffers(1, &res.handle);
        
        glBindBuffer(params.bind_flags, res.handle);
        
        glBufferData(params.bind_flags, params.buffer_size, params.data, params.usage_flags );
        
        res.type = params.bind_flags;
        
		return resource_index;
	}
    
    u32 direct::renderer_link_shader_program(const pen::shader_link_params &params )
    {
        u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
        
        GLuint vs = resource_pool[ params.vertex_shader ].handle;
        GLuint ps = resource_pool[ params.pixel_shader ].handle;
        
        shader_program* linked_program = link_program_internal( vs, ps );
        
        GLuint prog = linked_program->program;
        
        //build lookup tables for uniform buffers and texture samplers
        for( u32 i = 0; i < params.num_constants; ++i )
        {
            constant_layout_desc& constant = params.constants[i];
            GLint loc;
            
            switch( constant.type )
            {
                case pen::CT_CBUFFER:
                {
                    loc = glGetUniformBlockIndex(prog, constant.name);
                    if( loc != constant.location )
                    {
                        glUniformBlockBinding(prog, loc, constant.location);
                    }
                }
                break;
                case pen::CT_SAMPLER_2D:
                {
                    loc = glGetUniformLocation(prog, constant.name);
                    glUniform1i( loc, constant.location );
                }
                break;
                default:
                    break;
            }
        }
        
        resource_pool[ resource_index ].shader_program = linked_program;
        
        return resource_index;
    }
    
    void direct::renderer_set_so_target( u32 buffer_index )
    {
        
    }
    
    void direct::renderer_create_so_shader( const pen::shader_load_params &params )
    {
        
    }
    
    void direct::renderer_draw_auto( )
    {
        
    }

	u32 direct::renderer_create_input_layout( const input_layout_creation_params &params )
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
        
        resource_allocation& res = resource_pool[ resource_index ];
        
        res.input_layout = new input_layout;
    
        auto& attributes = res.input_layout->attributes;
        
        attributes.resize(params.num_elements);
        
        for( u32 i = 0; i < params.num_elements; ++i )
        {
            attributes[ i ].location        = i;
            attributes[ i ].type            = UNPACK_FORMAT(params.input_layout[ i ].format);
            attributes[ i ].num_elements    = UNPACK_NUM_ELEMENTS(params.input_layout[ i ].format);
            attributes[ i ].offset          = params.input_layout[ i ].aligned_byte_offset;
            attributes[ i ].stride          = 0;
        }

		return resource_index;
	}

	void direct::renderer_set_vertex_buffer( u32 buffer_index, u32 start_slot, u32 num_buffers, const u32* strides, const u32* offsets )
	{
        g_current_state.vertex_buffer = buffer_index;
        g_current_state.vertex_buffer_stride = strides[ 0 ];
        
        //todo support multiple vertex stream / instance data
	}

	void direct::renderer_set_input_layout( u32 layout_index )
	{
        g_current_state.input_layout = layout_index;
	}

	void direct::renderer_set_index_buffer( u32 buffer_index, u32 format, u32 offset )
	{
        g_bound_state.index_buffer = buffer_index;
	}
    
    void bind_state()
    {
        //bind shaders
        if( g_current_state.vertex_shader != g_bound_state.vertex_shader ||
            g_current_state.pixel_shader != g_bound_state.pixel_shader )
        {
            g_bound_state.vertex_shader = g_current_state.vertex_shader;
            g_bound_state.pixel_shader = g_current_state.pixel_shader;
            
            shader_program* linked_program = nullptr;
            
            for( auto program : shader_programs )
            {
                if( program.vs == g_bound_state.vertex_shader && program.vs == g_bound_state.vertex_shader )
                {
                    linked_program = &program;
                    break;
                }
            }
            
            if( linked_program == nullptr )
            {
                auto vs_handle = resource_pool[g_bound_state.vertex_shader].handle;
                auto ps_handle = resource_pool[g_bound_state.pixel_shader].handle;
                
                linked_program = link_program_internal(vs_handle, ps_handle);
            }
            
            glUseProgram( linked_program->program );
        }
        
        //bind vertex buffer
        {
            g_bound_state.vertex_buffer = g_current_state.vertex_buffer;
            
            auto& res = resource_pool[g_bound_state.vertex_buffer].handle;
            glBindBuffer(GL_ARRAY_BUFFER, res);
        }
        
        //bind input layout
        auto* input_res = resource_pool[g_current_state.input_layout].input_layout;

        //if input layout has changed, vb has changed or the stride of the vb has changed
        bool invalidate_input_layout = input_res->vb_handle == 0 || input_res->vb_handle != g_bound_state.vertex_buffer;
        invalidate_input_layout |= g_current_state.input_layout != g_bound_state.input_layout;
        invalidate_input_layout |= g_current_state.vertex_buffer_stride != g_bound_state.vertex_buffer_stride;
        
        if( invalidate_input_layout )
        {
            g_bound_state.input_layout = g_current_state.input_layout;
            g_bound_state.vertex_buffer_stride = g_current_state.vertex_buffer_stride;
            
            auto* res = input_res;
            
            //if we havent already generated one or we have previously been bound to a different vb layout
            if( res->vertex_array_handle == 0 || res->vb_handle != g_bound_state.vertex_buffer )
            {
                if( res->vertex_array_handle == 0 )
                {
                    glGenVertexArrays(1, &res->vertex_array_handle);
                }
                
                res->vb_handle = g_bound_state.vertex_buffer;
                
                glBindVertexArray(res->vertex_array_handle);
                
                for( auto& attribute : res->attributes )
                {
                    glVertexAttribPointer(
                                          attribute.location,
                                          attribute.num_elements,
                                          attribute.type,
                                          attribute.type == GL_UNSIGNED_BYTE ? true : false,
                                          g_bound_state.vertex_buffer_stride,
                                          (void*)attribute.offset);
                    
                    g_bound_state.enabled_vertex_attributes[attribute.location] = true;
                }
                        
                for( u32 i = 0; i < MAX_VERTEX_ATTRIBUTES; ++i )
                {
                    if( g_bound_state.enabled_vertex_attributes[i] )
                    {
                        glEnableVertexAttribArray(i);
                    }
                    else
                    {
                        glDisableVertexAttribArray(i);
                    }
                }
            }
            
            glBindVertexArray( res->vertex_array_handle );
        }

        if( g_bound_state.raster_state != g_current_state.raster_state )
        {
            g_bound_state.raster_state = g_current_state.raster_state;
            
            auto& rs = resource_pool[ g_bound_state.raster_state ].raster_state;
            
            glFrontFace(GL_CW);
            
            if( rs.culling_enabled )
            {
                glEnable( GL_CULL_FACE );
                glCullFace(rs.cull_face);
            }
            else
            {
                glDisable(GL_CULL_FACE);
            }
            
            if( rs.depth_clip_enabled )
            {
                glDisable(GL_DEPTH_CLAMP);
            }
            else
            {
                glEnable(GL_DEPTH_CLAMP);
            }
            
            glPolygonMode(GL_FRONT_AND_BACK, rs.polygon_mode);
            
            if( rs.scissor_enabled )
            {
                glEnable(GL_SCISSOR_TEST);
            }
            else
            {
                glDisable(GL_SCISSOR_TEST);
            }
        }
    }

	void direct::renderer_draw( u32 vertex_count, u32 start_vertex, u32 primitive_topology )
	{
        bind_state();
        
        glDrawArrays(primitive_topology, start_vertex, vertex_count);
	}

	void direct::renderer_draw_indexed( u32 index_count, u32 start_index, u32 base_vertex, u32 primitive_topology )
	{
        bind_state();
        
        //bind index buffer -this must always be re-bound
        {
            auto& res = resource_pool[g_bound_state.index_buffer].handle;
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, res);
        }
        
        glDrawElementsBaseVertex( primitive_topology, index_count, GL_UNSIGNED_SHORT, (void*)(size_t)(start_index * 2), base_vertex );
	}
    
    u32 calc_mip_level_size( u32 w, u32 h, u32 block_size, u32 pixels_per_block )
    {
        u32 num_blocks = (w * h) / pixels_per_block;
        u32 size = num_blocks * block_size;
        
        return size;
    }
    
    void get_texture_format( u32 pen_format, u32& sized_format, u32& format, u32& type )
    {
        //PEN_FORMAT_B8G8R8A8_UNORM       = 0,
        //PEN_FORMAT_BC1_UNORM            = 1,
        //PEN_FORMAT_BC2_UNORM            = 2,
        //PEN_FORMAT_BC3_UNORM            = 3,
        //PEN_FORMAT_BC4_UNORM            = 4,
        //PEN_FORMAT_BC5_UNORM            = 5
        
        switch(pen_format)
        {
            case PEN_TEX_FORMAT_BGRA8_UNORM:
                sized_format = GL_RGBA8;
                format = GL_BGRA;
                type = GL_UNSIGNED_BYTE;
                break;
                
            case PEN_TEX_FORMAT_RGBA8_UNORM:
                sized_format = GL_RGBA8;
                format = GL_RGBA;
                type = GL_UNSIGNED_BYTE;
                break;
                
            default:
                PEN_ASSERT( 0 );
                break;
        }
    }
    
    texture_info create_texture2d_internal(const texture_creation_params& tcp)
    {
        u32 sized_format, format, type;
        get_texture_format( tcp.format, sized_format, format, type );
        
        u32 mip_w = tcp.width;
        u32 mip_h = tcp.height;
        c8* mip_data = (c8*)tcp.data;
        
        GLuint handle;
        glGenTextures( 1, &handle);
        glBindTexture( GL_TEXTURE_2D, handle );
        
        for( u32 mip = 0; mip < tcp.num_mips; ++mip )
        {
            glTexImage2D(GL_TEXTURE_2D, mip, sized_format, mip_w, mip_h, 0, format, type, mip_data);
            
            mip_data += calc_mip_level_size(mip_w, mip_h, tcp.block_size, tcp.pixels_per_block);
            
            mip_w /= 2;
            mip_h /= 2;
        }
        
        glBindTexture(GL_TEXTURE_2D, 0 );
        
        texture_info ti;
        ti.handle = handle;
        ti.max_mip_level = tcp.num_mips - 1;
        
        return ti;
    }

	u32 direct::renderer_create_render_target(const texture_creation_params& tcp)
	{
		u32 resource_index = renderer_get_next_resource_index(DIRECT_RESOURCE);
        
        resource_allocation& res = resource_pool[ resource_index ];
        
        res.type = RES_RENDER_TARGET;
        
        glGenFramebuffers(1, &res.render_target.framebuffer );
        glBindFramebuffer(GL_FRAMEBUFFER, res.render_target.framebuffer);
        
        res.render_target.texture = create_texture2d_internal(tcp);
        
        glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, res.render_target.texture.handle, 0);
        
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        
        return resource_index;
	}

	void direct::renderer_set_targets( u32 colour_target, u32 depth_target, u32 colour_face, u32 depth_face )
	{
        if( colour_target == 0 )
        {
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        else
        {
            GLenum DrawBuffers[1] = {GL_COLOR_ATTACHMENT0};
            glDrawBuffers(1, DrawBuffers);
        
            resource_allocation& res = resource_pool[ colour_target ];
            
            glBindFramebuffer( GL_FRAMEBUFFER, res.render_target.framebuffer );
        }
	}

	u32 direct::renderer_create_texture2d(const texture_creation_params& tcp)
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
    
        resource_pool[ resource_index ].type = RES_TEXTURE;
        resource_pool[ resource_index ].texture = create_texture2d_internal( tcp );

		return resource_index;
	}

	u32 direct::renderer_create_sampler( const sampler_creation_params& scp )
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
        
        resource_pool[ resource_index ].sampler_state = (sampler_creation_params*)pen::memory_alloc(sizeof(scp));
        
        pen::memory_cpy(resource_pool[ resource_index ].sampler_state, &scp, sizeof(scp));

		return resource_index;
	}

	void direct::renderer_set_texture( u32 texture_index, u32 sampler_index, u32 resource_slot, u32 shader_type )
	{
        resource_allocation& res = resource_pool[ texture_index ];
        
        glActiveTexture(GL_TEXTURE0 + resource_slot);
        
        u32 max_mip = 0;
        
        if( res.type == RES_TEXTURE )
        {
            glBindTexture( GL_TEXTURE_2D, res.texture.handle );
            max_mip = res.texture.max_mip_level;
        }
        else
        {
            glBindTexture( GL_TEXTURE_2D, res.render_target.texture.handle );
            max_mip = res.render_target.texture.max_mip_level;
        }
        
        auto* sampler_state = resource_pool[sampler_index].sampler_state;
        
        //handle unmipped textures or textures with missisng mips
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max_mip);
        
        // filter
        switch( sampler_state->filter )
        {
            case PEN_FILTER_MIN_MAG_MIP_LINEAR:
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                break;
            case PEN_FILTER_MIN_MAG_MIP_POINT:
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_POINT);
                break;
            case PEN_FILTER_LINEAR:
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                break;
            case PEN_FILTER_POINT:
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_POINT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_POINT);
                break;
        };
        
        //address mode
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, sampler_state->address_u );
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, sampler_state->address_v );
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_R, sampler_state->address_w );
        
        //mip control
        glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, sampler_state->mip_lod_bias );
        
        if( sampler_state->max_lod > -1.0f )
        {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_LOD, sampler_state->max_lod );
        }
        
        if( sampler_state->min_lod > -1.0f )
        {
            glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_LOD, sampler_state->min_lod );
        }
	}

	u32 direct::renderer_create_rasterizer_state( const rasteriser_state_creation_params &rscp )
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
        
        auto& rs = resource_pool[resource_index].raster_state;
        
        rs = { 0 };
        
        if( rscp.cull_mode != PEN_CULL_NONE )
        {
            rs.culling_enabled = true;
            rs.cull_face = rscp.cull_mode;
        }
        
        rs.depth_clip_enabled = rscp.depth_clip_enable;
        rs.scissor_enabled = rscp.scissor_enable;
        
        rs.polygon_mode = rscp.fill_mode;

		return resource_index;
	}

	void direct::renderer_set_rasterizer_state( u32 rasterizer_state_index )
	{
        g_current_state.raster_state = rasterizer_state_index;
	}

    viewport g_current_vp;
	void direct::renderer_set_viewport( const viewport &vp )
	{
        g_current_vp = vp;
        
        glViewport( vp.x, vp.y, vp.width, vp.height );
        glDepthRangef( vp.min_depth, vp.max_depth );
	}
    
    void direct::renderer_set_scissor_rect( const rect &r )
    {
        f32 top = g_current_vp.height - r.bottom;
        glScissor(r.left, top, r.right - r.left, r.bottom - r.top);
    }

	u32 direct::renderer_create_blend_state( const blend_creation_params &bcp )
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
        
        resource_pool[ resource_index ].blend_state = (blend_creation_params*)pen::memory_alloc( sizeof(blend_creation_params) );
        
        blend_creation_params* blend_state = resource_pool[ resource_index ].blend_state;
        
        *blend_state = bcp;
        
        blend_state->render_targets = (render_target_blend*)pen::memory_alloc( sizeof(render_target_blend) * bcp.num_render_targets );
        
        for( s32 i = 0; i < bcp.num_render_targets; ++i )
        {
            blend_state->render_targets[i] = bcp.render_targets[i];
        }

		return resource_index;
	}

	void direct::renderer_set_blend_state( u32 blend_state_index )
	{
        auto* blend_state = resource_pool[ blend_state_index ].blend_state;
        
        for( s32 i = 0; i < blend_state->num_render_targets; ++i )
        {
            auto& rt_blend = blend_state->render_targets[ i ];
            
            if( i == 0 )
            {
                if( rt_blend.blend_enable )
                {
                    glEnable(GL_BLEND);
                    
                    if( blend_state->independent_blend_enable )
                    {
                        glBlendFuncSeparate(rt_blend.src_blend, rt_blend.dest_blend, rt_blend.src_blend_alpha, rt_blend.dest_blend_alpha);
                        glBlendEquationSeparate(rt_blend.blend_op, rt_blend.blend_op_alpha);
                    }
                    else
                    {
                        glBlendFunc(rt_blend.src_blend, rt_blend.dest_blend);
                        glBlendEquation(rt_blend.blend_op);
                    }
                }
                else
                {
                    glDisable(GL_BLEND);
                }
            }
        }
	}

	void direct::renderer_set_constant_buffer( u32 buffer_index, u32 resource_slot, u32 shader_type )
	{
        resource_allocation& res = resource_pool[ buffer_index ];
        
        glBindBufferBase(GL_UNIFORM_BUFFER, resource_slot, res.handle);
	}

	void direct::renderer_update_buffer( u32 buffer_index, const void* data, u32 data_size, u32 offset )
	{
        resource_allocation& res = resource_pool[ buffer_index ];
        
        glBindBuffer( res.type, res.handle );
        
        void* mapped_data = glMapBuffer( res.type, GL_WRITE_ONLY );
        
        if( mapped_data )
        {
            c8* mapped_offset = ((c8*)mapped_data) + offset;
            pen::memory_cpy(mapped_offset, data, data_size);
        }
        
        glUnmapBuffer( res.type );
        
        glBindBuffer( res.type, 0 );
	}

	u32 direct::renderer_create_depth_stencil_state( const depth_stencil_creation_params& dscp )
	{
		u32 resource_index = renderer_get_next_resource_index( DIRECT_RESOURCE );
    
        resource_pool[ resource_index ].depth_stencil = (depth_stencil_creation_params*)pen::memory_alloc(sizeof(dscp));
        
        pen::memory_cpy( resource_pool[ resource_index ].depth_stencil, &dscp, sizeof(dscp));

		return resource_index;
	}

	void direct::renderer_set_depth_stencil_state( u32 depth_stencil_state )
	{
        resource_allocation& res = resource_pool[ depth_stencil_state ];
        
        if( res.depth_stencil->depth_enable )
        {
            glEnable(GL_DEPTH_TEST);
        }
        else
        {
            glDisable(GL_DEPTH_TEST);
        }
        
        glDepthFunc(res.depth_stencil->depth_func);
        glDepthMask(res.depth_stencil->depth_write_mask);
	}

	void direct::renderer_release_shader( u32 shader_index, u32 shader_type )
    {
        resource_allocation& res = resource_pool[ shader_index ];
        glDeleteShader( res.handle );
        
        res.handle = 0;
        
        renderer_mark_resource_deleted( shader_index );
	}

	void direct::renderer_release_buffer( u32 buffer_index )
	{
        resource_allocation& res = resource_pool[ buffer_index ];
        glDeleteBuffers(1, &res.handle);
        
        res.handle = 0;
        
        renderer_mark_resource_deleted( buffer_index );
	}

	void direct::renderer_release_texture2d( u32 texture_index )
	{
        resource_allocation& res = resource_pool[ texture_index ];
        glDeleteTextures(1, &res.handle);
        
        res.handle = 0;
        
        renderer_mark_resource_deleted( texture_index );
	}

	void direct::renderer_release_raster_state( u32 raster_state_index )
	{
        renderer_mark_resource_deleted( raster_state_index );
	}

	void direct::renderer_release_blend_state( u32 blend_state )
	{
        resource_allocation& res = resource_pool[ blend_state ];
        
        pen::memory_free(res.blend_state);
        
        renderer_mark_resource_deleted( blend_state );
	}

	void direct::renderer_release_render_target( u32 render_target )
	{
        resource_allocation& res = resource_pool[ render_target ];
        glDeleteTextures( 1, &res.render_target.texture.handle );
        glDeleteFramebuffers( 1, &res.render_target.texture.handle );
        
        renderer_mark_resource_deleted( render_target );
	}

	void direct::renderer_release_input_layout( u32 input_layout )
	{
        resource_allocation& res = resource_pool[ input_layout ];
        
        pen::memory_free(res.input_layout);
        
        renderer_mark_resource_deleted( input_layout );
	}

	void direct::renderer_release_sampler( u32 sampler )
	{
        resource_allocation& res = resource_pool[ sampler ];
        
        pen::memory_free(res.sampler_state);
        
        renderer_mark_resource_deleted( sampler );
	}

	void direct::renderer_release_depth_stencil_state( u32 depth_stencil_state )
	{
        resource_allocation& res = resource_pool[ depth_stencil_state ];
        
        pen::memory_free( res.depth_stencil );
        
        renderer_mark_resource_deleted( depth_stencil_state );
	}
    
    void direct::renderer_release_clear_state( u32 clear_state )
    {
        renderer_mark_resource_deleted( clear_state );
    }
    
    void direct::renderer_release_program( u32 program )
    {
        resource_allocation& res = resource_pool[ program ];
        
        glDeleteProgram(res.shader_program->program);
        
        renderer_mark_resource_deleted( program );
    }

	void direct::renderer_release_query( u32 query )
	{

	}
    
	void renderer_update_queries()
	{

	}
    
    u32 direct::renderer_initialise( void* )
    {
        //todo renderer caps
        //const GLubyte* version = glGetString(GL_SHADING_LANGUAGE_VERSION);
        
        return PEN_ERR_OK;
    }
    
    void direct::renderer_shutdown( )
    {
        //todo device / stray resource shutdown
    }
    
    const c8* renderer_get_shader_platform( )
    {
        return "glsl";
    }
}
