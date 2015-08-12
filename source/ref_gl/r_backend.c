/*
Copyright (C) 2002-2011 Victor Luchits

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "r_local.h"
#include "r_backend_local.h"

#define COMPACT_STREAM_VATTRIBS ( VATTRIB_POSITION_BIT | VATTRIB_COLOR0_BIT | VATTRIB_TEXCOORDS_BIT )
#define CURRENT_VBO_IS_GENERIC_STREAM() ( ( rb.currentVBOId == RB_VBO_STREAM ) || ( rb.currentVBOId == RB_VBO_STREAM_COMPACT ) )
#define CURRENT_VBO_IS_QUAD_STREAM() ( ( rb.currentVBOId == RB_VBO_STREAM_QUAD ) || ( rb.currentVBOId == RB_VBO_STREAM_QUAD_COMPACT ) )

ATTRIBUTE_ALIGNED( 16 ) vec4_t batchVertsArray[MAX_BATCH_VERTS];
ATTRIBUTE_ALIGNED( 16 ) vec4_t batchNormalsArray[MAX_BATCH_VERTS];
ATTRIBUTE_ALIGNED( 16 ) vec4_t batchSVectorsArray[MAX_BATCH_VERTS];
ATTRIBUTE_ALIGNED( 16 ) vec2_t batchSTCoordsArray[MAX_BATCH_VERTS];
ATTRIBUTE_ALIGNED( 16 ) vec2_t batchLMCoordsArray[MAX_LIGHTMAPS][MAX_BATCH_VERTS];
ATTRIBUTE_ALIGNED( 16 ) byte_vec4_t batchLMLayersArray[( MAX_LIGHTMAPS + 3 ) / 4][MAX_BATCH_VERTS];
ATTRIBUTE_ALIGNED( 16 ) byte_vec4_t batchColorsArray[MAX_LIGHTMAPS][MAX_BATCH_VERTS];
ATTRIBUTE_ALIGNED( 16 ) elem_t batchElements[MAX_BATCH_ELEMENTS];

rbackend_t rb;

static void RB_InitBatchMesh( void );
static void RB_SetGLDefaults( void );
static void RB_RegisterStreamVBOs( void );
static void RB_UploadStaticQuadIndices( int id );

/*
* RB_Init
*/
void RB_Init( void )
{
	memset( &rb, 0, sizeof( rb ) );

	rb.mempool = R_AllocPool( NULL, "Rendering Backend" );

	// set default OpenGL state
	RB_SetGLDefaults();

	// initialize shading
	RB_InitShading();

	// intialize batching
	RB_InitBatchMesh();

	// create VBO's we're going to use for streamed data
	RB_RegisterStreamVBOs();

	// upload persistent quad indices
	RB_UploadStaticQuadIndices( RB_VBO_STREAM_QUAD );
	RB_UploadStaticQuadIndices( RB_VBO_STREAM_QUAD_COMPACT );
}

/*
* RB_Shutdown
*/
void RB_Shutdown( void )
{
	R_FreePool( &rb.mempool );
}

/*
* RB_BeginRegistration
*/
void RB_BeginRegistration( void )
{
	RB_RegisterStreamVBOs();
	RB_BindVBO( 0, 0 );
}

/*
* RB_EndRegistration
*/
void RB_EndRegistration( void )
{
	RB_BindVBO( 0, 0 );
}

/*
* RB_SetTime
*/
void RB_SetTime( unsigned int time )
{
	rb.time = time;
	rb.nullEnt.shaderTime = ri.Sys_Milliseconds();
}

/*
* RB_BeginFrame
*/
void RB_BeginFrame( void )
{
	Vector4Set( rb.nullEnt.shaderRGBA, 1, 1, 1, 1 );
	rb.nullEnt.scale = 1;
	VectorClear( rb.nullEnt.origin );
	Matrix3_Identity( rb.nullEnt.axis );

	memset( &rb.stats, 0, sizeof( rb.stats ) );

	// start fresh each frame
	RB_SetShaderStateMask( ~0, 0 );
	RB_BindVBO( 0, 0 );
}

/*
* RB_EndFrame
*/
void RB_EndFrame( void )
{
}

/*
* RB_StatsMessage
*/
void RB_StatsMessage( char *msg, size_t size )
{
	Q_snprintfz( msg, size, 
		"%4i verts %4i tris\n"
		"%4i draws %4i binds %4i progs",		
		rb.stats.c_totalVerts, rb.stats.c_totalTris,
		rb.stats.c_totalDraws, rb.stats.c_totalBinds, rb.stats.c_totalPrograms
	);
}

/*
* RB_SetGLDefaults
*/
static void RB_SetGLDefaults( void )
{
	if( glConfig.stencilBits )
	{
		qglStencilMask( ( GLuint ) ~0 );
		qglStencilFunc( GL_EQUAL, 128, 0xFF );
		qglStencilOp( GL_KEEP, GL_KEEP, GL_INCR );
	}

	qglDisable( GL_CULL_FACE );
	qglFrontFace( GL_CCW );
	qglDisable( GL_BLEND );
	qglDepthFunc( GL_LEQUAL );
	qglDepthMask( GL_FALSE );
	qglDisable( GL_POLYGON_OFFSET_FILL );
	qglPolygonOffset( -1.0f, 0.0f ); // units will be handled by RB_DepthOffset
	qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	qglEnable( GL_DEPTH_TEST );
#ifndef GL_ES_VERSION_2_0
	qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
#endif
	qglFrontFace( GL_CCW );
}

/*
* RB_BindTexture
*/
void RB_BindTexture( int tmu, const image_t *tex )
{
	if( R_BindTexture( tmu, tex ) ) {
		rb.stats.c_totalBinds++;
	}
}

/*
* RB_DepthRange
*/
void RB_DepthRange( float depthmin, float depthmax )
{
	clamp( depthmin, 0.0f, 1.0f );
	clamp( depthmax, 0.0f, 1.0f );
	rb.gl.depthmin = depthmin;
	rb.gl.depthmax = depthmax;
	// depthmin == depthmax is a special case when a specific depth value is going to be written
	if( ( depthmin != depthmax ) && !rb.gl.depthoffset )
		depthmin += 4.0f / 65535.0f;
	qglDepthRange( depthmin, depthmax );
}

/*
* RB_GetDepthRange
*/
void RB_GetDepthRange( float* depthmin, float *depthmax )
{
	*depthmin = rb.gl.depthmin;
	*depthmax = rb.gl.depthmax;
}

/*
* RB_DepthOffset
*/
void RB_DepthOffset( bool enable )
{
	float depthmin = rb.gl.depthmin;
	float depthmax = rb.gl.depthmax;
	rb.gl.depthoffset = enable;
	if( depthmin != depthmax )
	{
		if( !enable )
			depthmin += 4.0f / 65535.0f;
		qglDepthRange( depthmin, depthmax );
	}
}

/*
* RB_ClearDepth
*/
void RB_ClearDepth( float depth )
{
	qglClearDepth( depth );
}

/*
* RB_LoadCameraMatrix
*/
void RB_LoadCameraMatrix( const mat4_t m )
{
	Matrix4_Copy( m, rb.cameraMatrix );
}

/*
* RB_LoadObjectMatrix
*/
void RB_LoadObjectMatrix( const mat4_t m )
{
	Matrix4_Copy( m, rb.objectMatrix );
	Matrix4_MultiplyFast( rb.cameraMatrix, m, rb.modelviewMatrix );
	Matrix4_Multiply( rb.projectionMatrix, rb.modelviewMatrix, rb.modelviewProjectionMatrix );
}

/*
* RB_LoadProjectionMatrix
*/
void RB_LoadProjectionMatrix( const mat4_t m )
{
	Matrix4_Copy( m, rb.projectionMatrix );
	Matrix4_Multiply( m, rb.modelviewMatrix, rb.modelviewProjectionMatrix );
}

/*
* RB_Cull
*/
void RB_Cull( int cull )
{
	if( rb.gl.faceCull == cull )
		return;

	if( !cull )
	{
		qglDisable( GL_CULL_FACE );
		rb.gl.faceCull = 0;
		return;
	}

	if( !rb.gl.faceCull )
		qglEnable( GL_CULL_FACE );
	qglCullFace( cull );
	rb.gl.faceCull = cull;
}

/*
* RB_SetState
*/
void RB_SetState( int state )
{
	int diff;

	diff = rb.gl.state ^ state;
	if( !diff )
		return;

	if( diff & GLSTATE_BLEND_MASK )
	{
		if( state & GLSTATE_BLEND_MASK )
		{
			int blendsrc, blenddst;

			switch( state & GLSTATE_SRCBLEND_MASK )
			{
			case GLSTATE_SRCBLEND_ZERO:
				blendsrc = GL_ZERO;
				break;
			case GLSTATE_SRCBLEND_DST_COLOR:
				blendsrc = GL_DST_COLOR;
				break;
			case GLSTATE_SRCBLEND_ONE_MINUS_DST_COLOR:
				blendsrc = GL_ONE_MINUS_DST_COLOR;
				break;
			case GLSTATE_SRCBLEND_SRC_ALPHA:
				blendsrc = GL_SRC_ALPHA;
				break;
			case GLSTATE_SRCBLEND_ONE_MINUS_SRC_ALPHA:
				blendsrc = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case GLSTATE_SRCBLEND_DST_ALPHA:
				blendsrc = GL_DST_ALPHA;
				break;
			case GLSTATE_SRCBLEND_ONE_MINUS_DST_ALPHA:
				blendsrc = GL_ONE_MINUS_DST_ALPHA;
				break;
			default:
			case GLSTATE_SRCBLEND_ONE:
				blendsrc = GL_ONE;
				break;
			}

			switch( state & GLSTATE_DSTBLEND_MASK )
			{
			case GLSTATE_DSTBLEND_ONE:
				blenddst = GL_ONE;
				break;
			case GLSTATE_DSTBLEND_SRC_COLOR:
				blenddst = GL_SRC_COLOR;
				break;
			case GLSTATE_DSTBLEND_ONE_MINUS_SRC_COLOR:
				blenddst = GL_ONE_MINUS_SRC_COLOR;
				break;
			case GLSTATE_DSTBLEND_SRC_ALPHA:
				blenddst = GL_SRC_ALPHA;
				break;
			case GLSTATE_DSTBLEND_ONE_MINUS_SRC_ALPHA:
				blenddst = GL_ONE_MINUS_SRC_ALPHA;
				break;
			case GLSTATE_DSTBLEND_DST_ALPHA:
				blenddst = GL_DST_ALPHA;
				break;
			case GLSTATE_DSTBLEND_ONE_MINUS_DST_ALPHA:
				blenddst = GL_ONE_MINUS_DST_ALPHA;
				break;
			default:
			case GLSTATE_DSTBLEND_ZERO:
				blenddst = GL_ZERO;
				break;
			}

			if( !( rb.gl.state & GLSTATE_BLEND_MASK ) )
				qglEnable( GL_BLEND );

			qglBlendFuncSeparateEXT( blendsrc, blenddst, GL_ONE, GL_ONE );
		}
		else
		{
			qglDisable( GL_BLEND );
		}
	}

	if( diff & (GLSTATE_NO_COLORWRITE|GLSTATE_ALPHAWRITE) )
	{
		if( state & GLSTATE_NO_COLORWRITE )
			qglColorMask( GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE );
		else
			qglColorMask( GL_TRUE, GL_TRUE, GL_TRUE, ( state & GLSTATE_ALPHAWRITE ) ? GL_TRUE : GL_FALSE );
	}

	if( diff & (GLSTATE_DEPTHFUNC_EQ|GLSTATE_DEPTHFUNC_GT) )
	{
		if( state & GLSTATE_DEPTHFUNC_EQ )
			qglDepthFunc( GL_EQUAL );
		else if( state & GLSTATE_DEPTHFUNC_GT )
			qglDepthFunc( GL_GREATER );
		else
			qglDepthFunc( GL_LEQUAL );
	}

	if( diff & GLSTATE_DEPTHWRITE )
	{
		if( state & GLSTATE_DEPTHWRITE )
			qglDepthMask( GL_TRUE );
		else
			qglDepthMask( GL_FALSE );
	}

	if( diff & GLSTATE_NO_DEPTH_TEST )
	{
		if( state & GLSTATE_NO_DEPTH_TEST )
			qglDisable( GL_DEPTH_TEST );
		else
			qglEnable( GL_DEPTH_TEST );
	}

	if( diff & GLSTATE_OFFSET_FILL )
	{
		if( state & GLSTATE_OFFSET_FILL )
		{
			qglEnable( GL_POLYGON_OFFSET_FILL );
			RB_DepthOffset( true );
		}
		else
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
			RB_DepthOffset( false );
		}
	}

	if( diff & GLSTATE_STENCIL_TEST )
	{
		if( glConfig.stencilBits )
		{
			if( state & GLSTATE_STENCIL_TEST )
				qglEnable( GL_STENCIL_TEST );
			else
				qglDisable( GL_STENCIL_TEST );
		}
	}

	rb.gl.state = state;
}

/*
* RB_FrontFace
*/
void RB_FrontFace( bool front )
{
	qglFrontFace( front ? GL_CW : GL_CCW );
	rb.gl.frontFace = front;
}

/*
* RB_FlipFrontFace
*/
void RB_FlipFrontFace( void )
{
	RB_FrontFace( !rb.gl.frontFace );
}

/*
* RB_BindArrayBuffer
*/
void RB_BindArrayBuffer( int buffer )
{
	if( buffer != rb.gl.currentArrayVBO )
	{
		qglBindBufferARB( GL_ARRAY_BUFFER_ARB, buffer );
		rb.gl.currentArrayVBO = buffer;
		rb.gl.lastVAttribs = 0;
	}
}

/*
* RB_BindElementArrayBuffer
*/
void RB_BindElementArrayBuffer( int buffer )
{
	if( buffer != rb.gl.currentElemArrayVBO )
	{
		qglBindBufferARB( GL_ELEMENT_ARRAY_BUFFER_ARB, buffer );
		rb.gl.currentElemArrayVBO = buffer;
	}
}

/*
* GL_EnableVertexAttrib
*/
static void GL_EnableVertexAttrib( int index, bool enable )
{
	unsigned int bit;
	unsigned int diff;

	bit = 1 << index;
	diff = (rb.gl.vertexAttribEnabled & bit) ^ (enable ? bit : 0);
	if( !diff ) {
		return;
	}

	if( enable ) {
		rb.gl.vertexAttribEnabled |= bit;
		qglEnableVertexAttribArrayARB( index );
	}
	else {
		rb.gl.vertexAttribEnabled &= ~bit;
		qglDisableVertexAttribArrayARB( index );
	}
}

/*
* RB_Scissor
*/
void RB_Scissor( int x, int y, int w, int h )
{
	qglScissor( x, rb.gl.fbHeight - h - y, w, h );

	rb.gl.scissor[0] = x;
	rb.gl.scissor[1] = y;
	rb.gl.scissor[2] = w;
	rb.gl.scissor[3] = h;
}

/*
* RB_GetScissor
*/
void RB_GetScissor( int *x, int *y, int *w, int *h )
{
	if( x ) {
		*x = rb.gl.scissor[0];
	}
	if( y ) {
		*y = rb.gl.scissor[1];
	}
	if( w ) {
		*w = rb.gl.scissor[2];
	}
	if( h ) {
		*h = rb.gl.scissor[3];
	}
}

/*
* RB_EnableScissor
*/
void RB_EnableScissor( bool enable )
{
	if( enable ) {
		qglEnable( GL_SCISSOR_TEST );
	}
	else {
		qglDisable( GL_SCISSOR_TEST );
	}
}

/*
* RB_Viewport
*/
void RB_Viewport( int x, int y, int w, int h )
{
	rb.gl.viewport[0] = x;
	rb.gl.viewport[1] = y;
	rb.gl.viewport[2] = w;
	rb.gl.viewport[3] = h;
	qglViewport( x, rb.gl.fbHeight - h - y, w, h );
}

/*
* RB_Clear
*/
void RB_Clear( int bits, float r, float g, float b, float a )
{
	int state = rb.gl.state;

	if( bits & GL_DEPTH_BUFFER_BIT )
		state |= GLSTATE_DEPTHWRITE;

	if( bits & GL_STENCIL_BUFFER_BIT )
		qglClearStencil( 128 );

	if( bits & GL_COLOR_BUFFER_BIT )
	{
		state = ( state & ~GLSTATE_NO_COLORWRITE ) | GLSTATE_ALPHAWRITE;
		qglClearColor( r, g, b, a );
	}

	RB_SetState( state );

	qglClear( bits );

	RB_DepthRange( 0.0f, 1.0f );
}

/*
* RB_BindFrameBufferObject
*/
void RB_BindFrameBufferObject( int object )
{
	int width, height;

	RFB_BindObject( object );

	RFB_GetObjectSize( object, &width, &height );

	rb.gl.fbWidth = width;
	rb.gl.fbHeight = height;
}

/*
* RB_BoundFrameBufferObject
*/
int RB_BoundFrameBufferObject( void )
{
	return RFB_BoundObject();
}

/*
* RB_BlitFrameBufferObject
*/
void RB_BlitFrameBufferObject( int dest, int bitMask, int mode )
{
	RFB_BlitObject( dest, bitMask, mode );
}

/*
* RB_UploadStaticQuadIndices
*/
static void RB_UploadStaticQuadIndices( int id )
{
	int leftVerts, numVerts, numElems;
	int vertsOffset, elemsOffset;
	mesh_t mesh;
	mesh_vbo_t *vbo = rb.streamVBOs[-id - 1];

	assert( MAX_BATCH_VERTS <= MAX_STREAM_VBO_VERTS );

	vertsOffset = 0;
	elemsOffset = 0;
	
	memset( &mesh, 0, sizeof( mesh ) );

	for( leftVerts = MAX_STREAM_VBO_VERTS; leftVerts > 0; leftVerts -= numVerts ) {
		numVerts = min( MAX_BATCH_VERTS, leftVerts );
		numElems = numVerts/4*6;

		mesh.numElems = numElems;
		mesh.numVerts = numVerts;

		R_UploadVBOElemData( vbo, vertsOffset, elemsOffset, &mesh, VBO_HINT_ELEMS_QUAD );
		vertsOffset += numVerts;
		elemsOffset += numElems;
	}
}

/*
* RB_RegisterStreamVBOs
*
* Allocate/keep alive dynamic vertex buffers object 
* we'll steam the dynamic geometry into
*/
void RB_RegisterStreamVBOs( void )
{
	int i;
	mesh_vbo_t *vbo;
	vbo_tag_t tags[RB_VBO_NUM_STREAMS] = {
		VBO_TAG_STREAM,
		VBO_TAG_STREAM,
		VBO_TAG_STREAM_STATIC_ELEMS,
		VBO_TAG_STREAM_STATIC_ELEMS
	};
	vattribmask_t vattribs[RB_VBO_NUM_STREAMS] = {
		VATTRIBS_MASK,
		COMPACT_STREAM_VATTRIBS,
		VATTRIBS_MASK,
		COMPACT_STREAM_VATTRIBS
	};

	// allocate stream VBO's
	for( i = 0; i < RB_VBO_NUM_STREAMS; i++ ) {
		vbo = rb.streamVBOs[i];
		if( vbo ) {
			R_TouchMeshVBO( vbo );
			continue;
		}
		rb.streamVBOs[i] = R_CreateMeshVBO( &rb, 
			MAX_STREAM_VBO_VERTS, MAX_STREAM_VBO_ELEMENTS, MAX_STREAM_VBO_INSTANCES,
			vattribs[i], tags[i], VATTRIB_TEXCOORDS_BIT|VATTRIB_NORMAL_BIT|VATTRIB_SVECTOR_BIT );
	}
}

/*
* RB_InitBatchMesh
*/
static void RB_InitBatchMesh( void )
{
	int i;
	mesh_t *mesh = &rb.batchMesh;

	mesh->numVerts = 0;
	mesh->numElems = 0;
	mesh->xyzArray = batchVertsArray;
	mesh->normalsArray = batchNormalsArray;
	mesh->sVectorsArray = batchSVectorsArray;
	mesh->stArray = batchSTCoordsArray;
	for( i = 0; i < MAX_LIGHTMAPS; i++ ) {
		mesh->lmstArray[i] = batchLMCoordsArray[i];
		mesh->colorsArray[i] = batchColorsArray[i];
		if( !( i & 3 ) ) {
			mesh->lmlayersArray[i >> 2] = batchLMLayersArray[i >> 2];
		}
	}
	mesh->elems = batchElements;
}

/*
* RB_BindVBO
*/
void RB_BindVBO( int id, int primitive )
{
	mesh_vbo_t *vbo;
	rbDrawElements_t *batch;

	if( !( rb.currentVAttribs & ~COMPACT_STREAM_VATTRIBS ) ) {
		if( id == RB_VBO_STREAM ) {
			id = RB_VBO_STREAM_COMPACT;
		} else if( id == RB_VBO_STREAM_QUAD ) {
			id = RB_VBO_STREAM_QUAD_COMPACT;
		}
	}

	rb.primitive = primitive;

	if( rb.currentVBOId == id ) {
		return;
	}

	if( id < RB_VBO_NONE ) {
		vbo = rb.streamVBOs[-id - 1];
		batch = &rb.batches[-id - 1];
	} else if( id == RB_VBO_NONE ) {
		vbo = NULL;
		batch = NULL;
	}
	else {
		vbo = R_GetVBOByIndex( id );
		batch = NULL;
	}

	rb.currentVBOId = id;
	rb.currentVBO = vbo;
	rb.currentBatch = batch;
	if( !vbo ) {
		RB_BindArrayBuffer( 0 );
		RB_BindElementArrayBuffer( 0 );
		return;
	}

	RB_BindArrayBuffer( vbo->vertexId );
	RB_BindElementArrayBuffer( vbo->elemId );
}

/*
* RB_UploadMesh
*/
void RB_UploadMesh( const mesh_t *mesh )
{
	int stream;
	mesh_vbo_t *vbo;
	rbDrawElements_t *offset;
	vbo_hint_t vbo_hint = VBO_HINT_NONE;
	int numVerts = mesh->numVerts, numElems = mesh->numElems;
	bool isQuadStream, isGenericStream;

	assert( rb.currentVBOId < RB_VBO_NONE );
	if( rb.currentVBOId >= RB_VBO_NONE ) {
		return;
	}
	
	isQuadStream = CURRENT_VBO_IS_QUAD_STREAM();
	isGenericStream = CURRENT_VBO_IS_GENERIC_STREAM( );

	if( isQuadStream ) {
		numElems = numVerts/4*6;
	} else if( !numElems && isGenericStream ) {
		numElems = (max(numVerts, 2) - 2) * 3;
	}

	if( !numVerts || !numElems ) {
		return;
	}

	vbo = rb.currentVBO;
	stream = -rb.currentVBOId - 1;
	offset = &rb.streamOffset[stream];

	if( offset->firstVert+offset->numVerts+numVerts > MAX_STREAM_VBO_VERTS || 
		offset->firstElem+offset->numVerts+numElems > MAX_STREAM_VBO_ELEMENTS ) {

		RB_DrawElements( offset->firstVert, offset->numVerts, offset->firstElem, offset->numElems, 
			offset->firstVert, offset->numVerts, offset->firstElem, offset->numElems );

		R_DiscardVBOVertexData( vbo );
		if( !isQuadStream ) {
			R_DiscardVBOElemData( vbo );
		}

		offset->firstVert = 0;
		offset->firstElem = 0;
		offset->numVerts = 0;
		offset->numElems = 0;
	}

	if( numVerts > MAX_STREAM_VBO_VERTS ||
		numElems > MAX_STREAM_VBO_ELEMENTS ) {
		// FIXME: do something about this?
		return;
	}

	if( isQuadStream ) {
		vbo_hint = VBO_HINT_ELEMS_QUAD;

		// quad indices are stored in a static vbo, don't call R_UploadVBOElemData
	} else {
		if( mesh->elems ) {
			vbo_hint = VBO_HINT_NONE;
		} else if( isGenericStream ) {
			vbo_hint = VBO_HINT_ELEMS_TRIFAN;
		} else {
			assert( 0 );
		}
		R_UploadVBOElemData( vbo, offset->firstVert + offset->numVerts, 
			offset->firstElem + offset->numElems, mesh, vbo_hint );
	}

	R_UploadVBOVertexData( vbo, offset->firstVert + offset->numVerts, 
		rb.currentVAttribs, mesh, vbo_hint );

	offset->numElems += numElems;
	offset->numVerts += numVerts;
}

/*
* RB_UploadBatchMesh
*/
static void RB_UploadBatchMesh( rbDrawElements_t *batch )
{
	rb.batchMesh.numVerts = batch->numVerts;
	rb.batchMesh.numElems = batch->numElems;

	RB_UploadMesh( &rb.batchMesh );

	batch->numElems = batch->numVerts = 0;
	batch->firstElem = batch->firstVert = 0;
}

/*
* RB_MapBatchMesh
*/
mesh_t *RB_MapBatchMesh( int numVerts, int numElems )
{
	int stream;
	rbDrawElements_t *batch;

	assert( rb.currentVBOId < RB_VBO_NONE );
	if( rb.currentVBOId >= RB_VBO_NONE ) {
		return NULL;
	}

	if( numVerts > MAX_BATCH_VERTS || 
		numElems > MAX_BATCH_ELEMENTS ) {
		return NULL;
	}

	stream = -rb.currentVBOId - 1;
	batch = &rb.batches[stream];

	batch->numElems = batch->numVerts = 0;
	batch->firstElem = batch->firstVert = 0;

	RB_InitBatchMesh();

	return &rb.batchMesh;
}

/*
* RB_BeginBatch
*/
void RB_BeginBatch( void )
{
	RB_MapBatchMesh( 0, 0 );
}

/*
* RB_BatchMesh
*/
void RB_BatchMesh( const mesh_t *mesh )
{
	int stream;
	rbDrawElements_t *batch;
	int numVerts = mesh->numVerts, numElems = mesh->numElems;
	bool isQuadStream, isGenericStream;

	isQuadStream = CURRENT_VBO_IS_QUAD_STREAM();
	isGenericStream = CURRENT_VBO_IS_GENERIC_STREAM( );

	if( isQuadStream ) {
		numElems = numVerts/4*6;
	} else if( !numElems && isGenericStream ) {
		numElems = (max(numVerts, 2) - 2) * 3;
	}

	if( !numVerts || !numElems ) {
		return;
	}

	assert( rb.currentVBOId < RB_VBO_NONE );
	if( rb.currentVBOId >= RB_VBO_NONE ) {
		return;
	}

	stream = -rb.currentVBOId - 1;
	batch = &rb.batches[stream];

	if( numVerts+batch->numVerts > MAX_BATCH_VERTS || 
		numElems+batch->numElems > MAX_BATCH_ELEMENTS ) {
		RB_UploadBatchMesh( batch );
	}

	if( numVerts > MAX_BATCH_VERTS || 
		numElems > MAX_BATCH_ELEMENTS ) {
		RB_UploadMesh( mesh );
	}
	else {
		int i;
		vattribmask_t vattribs = rb.currentVAttribs;

		memcpy( rb.batchMesh.xyzArray + batch->numVerts, mesh->xyzArray, numVerts * sizeof( vec4_t ) );
		if( isQuadStream ) {
			// quad indices are stored in a static vbo
		} else if( mesh->elems ) {
			if( rb.primitive == GL_TRIANGLES ) {
				R_CopyOffsetTriangles( mesh->elems, numElems, batch->numVerts, rb.batchMesh.elems + batch->numElems );
			}
			else {
				R_CopyOffsetElements( mesh->elems, numElems, batch->numVerts, rb.batchMesh.elems + batch->numElems );
			}
		} else if( isGenericStream ) {
			R_BuildTrifanElements( batch->numVerts, numElems, rb.batchMesh.elems + batch->numElems );
		} else {
			assert( 0 );
		}
		if( mesh->normalsArray && (vattribs & VATTRIB_NORMAL_BIT) ) {
			memcpy( rb.batchMesh.normalsArray + batch->numVerts, mesh->normalsArray, numVerts * sizeof( vec4_t ) );
		}
		if( mesh->sVectorsArray && ( ( vattribs & (VATTRIB_SVECTOR_BIT|VATTRIB_AUTOSPRITE2_BIT) ) == VATTRIB_SVECTOR_BIT ) ) {
			memcpy( rb.batchMesh.sVectorsArray + batch->numVerts, mesh->sVectorsArray, numVerts * sizeof( vec4_t ) );
		}
		if( mesh->stArray && (vattribs & VATTRIB_TEXCOORDS_BIT) ) {
			memcpy( rb.batchMesh.stArray + batch->numVerts, mesh->stArray, numVerts * sizeof( vec2_t ) );
		}
		
		if( mesh->lmstArray[0] && (vattribs & VATTRIB_LMCOORDS0_BIT) ) {
			memcpy( rb.batchMesh.lmstArray[0] + batch->numVerts, mesh->lmstArray[0], numVerts * sizeof( vec2_t ) );
			if( mesh->lmlayersArray[0] && ( vattribs & VATTRIB_LMLAYERS0123_BIT ) ) {
				memcpy( rb.batchMesh.lmlayersArray[0] + batch->numVerts, mesh->lmlayersArray[0], numVerts * sizeof( byte_vec4_t ) );
			}

			for( i = 1; i < MAX_LIGHTMAPS; i++ ) {
				if( !mesh->lmstArray[i] || !(vattribs & (VATTRIB_LMCOORDS1_BIT<<(i-1))) ) {
					break;
				}
				memcpy( rb.batchMesh.lmstArray[i] + batch->numVerts, mesh->lmstArray[i], numVerts * sizeof( vec2_t ) );
				if( !( i & 3 ) && mesh->lmlayersArray[i >> 2] && ( vattribs & ( VATTRIB_LMLAYERS0123_BIT << ( i >> 2 ) ) ) ) {
					memcpy( rb.batchMesh.lmlayersArray[i >> 2] + batch->numVerts,
						mesh->lmlayersArray[i >> 2], numVerts * sizeof( byte_vec4_t ) );
				}
			}
		}

		if( mesh->colorsArray[0] && (vattribs & VATTRIB_COLOR0_BIT) ) {
			memcpy( rb.batchMesh.colorsArray[0] + batch->numVerts, mesh->colorsArray[0], numVerts * sizeof( byte_vec4_t ) );
		}

		batch->numVerts += numVerts;
		batch->numElems += numElems;
	}
}

/*
* RB_EndBatch
*/
void RB_EndBatch( void )
{
	int stream;
	rbDrawElements_t *batch;
	rbDrawElements_t *offset;

	if( rb.currentVBOId >= RB_VBO_NONE ) {
		return;
	}

	stream = -rb.currentVBOId - 1;
	offset = &rb.streamOffset[stream];
	batch = &rb.batches[stream];

	if( batch->numVerts ) {
		RB_UploadBatchMesh( batch );
	}

	if( !offset->numVerts || !offset->numElems ) {
		return;
	}

	RB_DrawElements( offset->firstVert, offset->numVerts, offset->firstElem, offset->numElems,
		offset->firstVert, offset->numVerts, offset->firstElem, offset->numElems );

	offset->firstVert += offset->numVerts;
	offset->firstElem += offset->numElems;
	offset->numVerts = offset->numElems = 0;
}

/*
* RB_EnableVertexAttribs
*/
static void RB_EnableVertexAttribs( void )
{
	vattribmask_t vattribs = rb.currentVAttribs;
	mesh_vbo_t *vbo = rb.currentVBO;
	vattribmask_t hfa = vbo->halfFloatAttribs;

	assert( vattribs & VATTRIB_POSITION_BIT );

	if( ( vattribs == rb.gl.lastVAttribs ) && ( hfa == rb.gl.lastHalfFloatVAttribs ) ) {
		return;
	}

	rb.gl.lastVAttribs = vattribs;
	rb.gl.lastHalfFloatVAttribs = hfa;

	// xyz position
	GL_EnableVertexAttrib( VATTRIB_POSITION, true );
	qglVertexAttribPointerARB( VATTRIB_POSITION, 4, FLOAT_VATTRIB_GL_TYPE( VATTRIB_POSITION_BIT, hfa ), 
		GL_FALSE, vbo->vertexSize, ( const GLvoid * )0 );

	// normal
	if( vattribs & VATTRIB_NORMAL_BIT ) {
		GL_EnableVertexAttrib( VATTRIB_NORMAL, true );
		qglVertexAttribPointerARB( VATTRIB_NORMAL, 4, FLOAT_VATTRIB_GL_TYPE( VATTRIB_NORMAL_BIT, hfa ), 
			GL_FALSE, vbo->vertexSize, ( const GLvoid * )vbo->normalsOffset );
	}
	else {
		GL_EnableVertexAttrib( VATTRIB_NORMAL, false );
	}

	// s-vector
	if( vattribs & VATTRIB_SVECTOR_BIT ) {
		GL_EnableVertexAttrib( VATTRIB_SVECTOR, true );
		qglVertexAttribPointerARB( VATTRIB_SVECTOR, 4, FLOAT_VATTRIB_GL_TYPE( VATTRIB_SVECTOR_BIT, hfa ), 
			GL_FALSE, vbo->vertexSize, ( const GLvoid * )vbo->sVectorsOffset );
	}
	else {
		GL_EnableVertexAttrib( VATTRIB_SVECTOR, false );
	}
	
	// color
	if( vattribs & VATTRIB_COLOR0_BIT ) {
		GL_EnableVertexAttrib( VATTRIB_COLOR0, true );
		qglVertexAttribPointerARB( VATTRIB_COLOR0, 4, GL_UNSIGNED_BYTE, 
			GL_TRUE, vbo->vertexSize, (const GLvoid * )vbo->colorsOffset[0] );
	}
	else {
		GL_EnableVertexAttrib( VATTRIB_COLOR0, false );
	}

	// texture coordinates
	if( vattribs & VATTRIB_TEXCOORDS_BIT ) {
		GL_EnableVertexAttrib( VATTRIB_TEXCOORDS, true );
		qglVertexAttribPointerARB( VATTRIB_TEXCOORDS, 2, FLOAT_VATTRIB_GL_TYPE( VATTRIB_TEXCOORDS_BIT, hfa ), 
			GL_FALSE, vbo->vertexSize, ( const GLvoid * )vbo->stOffset );
	}
	else {
		GL_EnableVertexAttrib( VATTRIB_TEXCOORDS, false );
	}

	if( (vattribs & VATTRIB_AUTOSPRITE_BIT) == VATTRIB_AUTOSPRITE_BIT ) {
		// submit sprite point
		GL_EnableVertexAttrib( VATTRIB_SPRITEPOINT, true );
		qglVertexAttribPointerARB( VATTRIB_SPRITEPOINT, 4, FLOAT_VATTRIB_GL_TYPE( VATTRIB_AUTOSPRITE_BIT, hfa ), 
			GL_FALSE, vbo->vertexSize, ( const GLvoid * )vbo->spritePointsOffset );
	}
	else {
		GL_EnableVertexAttrib( VATTRIB_SPRITEPOINT, false );
	}

	// bones (skeletal models)
	if( (vattribs & VATTRIB_BONES_BITS) == VATTRIB_BONES_BITS ) {
		// submit indices
		GL_EnableVertexAttrib( VATTRIB_BONESINDICES, true );
		qglVertexAttribPointerARB( VATTRIB_BONESINDICES, 4, GL_UNSIGNED_BYTE, 
			GL_FALSE, vbo->vertexSize, ( const GLvoid * )vbo->bonesIndicesOffset );

		// submit weights
		GL_EnableVertexAttrib( VATTRIB_BONESWEIGHTS, true );
		qglVertexAttribPointerARB( VATTRIB_BONESWEIGHTS, 4, GL_UNSIGNED_BYTE, 
			GL_TRUE, vbo->vertexSize, ( const GLvoid * )vbo->bonesWeightsOffset );
	}
	else {
		int i;
		vattrib_t lmattr;
		vattribbit_t lmattrbit;

		// lightmap texture coordinates - aliasing bones, so not disabling bones
		lmattr = VATTRIB_LMCOORDS01;
		lmattrbit = VATTRIB_LMCOORDS0_BIT;

		for( i = 0; i < ( MAX_LIGHTMAPS + 1 ) / 2; i++ ) {
			if( vattribs & lmattrbit ) {
				GL_EnableVertexAttrib( lmattr, true );
				qglVertexAttribPointerARB( lmattr, vbo->lmstSize[i], 
					FLOAT_VATTRIB_GL_TYPE( VATTRIB_LMCOORDS0_BIT, hfa ), 
					GL_FALSE, vbo->vertexSize, ( const GLvoid * )vbo->lmstOffset[i] );
			}
			else {
				GL_EnableVertexAttrib( lmattr, false );
			}

			lmattr++;
			lmattrbit <<= 2;
		}

		// lightmap array texture layers
		lmattr = VATTRIB_LMLAYERS0123;

		for( i = 0; i < ( MAX_LIGHTMAPS + 3 ) / 4; i++ ) {
			if( vattribs & ( VATTRIB_LMLAYERS0123_BIT << i ) ) {
				GL_EnableVertexAttrib( lmattr, true );
				qglVertexAttribPointerARB( lmattr, 4, GL_UNSIGNED_BYTE,
					GL_FALSE, vbo->vertexSize, ( const GLvoid * )vbo->lmlayersOffset[i] );
			}
			else {
				GL_EnableVertexAttrib( lmattr, false );
			}

			lmattr++;
		}
	}

	if( (vattribs & VATTRIB_INSTANCES_BITS) == VATTRIB_INSTANCES_BITS ) {
		GL_EnableVertexAttrib( VATTRIB_INSTANCE_QUAT, true );
		qglVertexAttribPointerARB( VATTRIB_INSTANCE_QUAT, 4, GL_FLOAT, GL_FALSE, 8 * sizeof( vec_t ), 
			( const GLvoid * )vbo->instancesOffset );
		qglVertexAttribDivisorARB( VATTRIB_INSTANCE_QUAT, 1 );

		GL_EnableVertexAttrib( VATTRIB_INSTANCE_XYZS, true );
		qglVertexAttribPointerARB( VATTRIB_INSTANCE_XYZS, 4, GL_FLOAT, GL_FALSE, 8 * sizeof( vec_t ), 
			( const GLvoid * )( vbo->instancesOffset + sizeof( vec_t ) * 4 ) );
		qglVertexAttribDivisorARB( VATTRIB_INSTANCE_XYZS, 1 );
	} else {
		GL_EnableVertexAttrib( VATTRIB_INSTANCE_QUAT, false );
		GL_EnableVertexAttrib( VATTRIB_INSTANCE_XYZS, false );
	}
}

/*
* RB_DrawElementsReal
*/
void RB_DrawElementsReal( rbDrawElements_t *de )
{
	int firstVert, numVerts, firstElem, numElems;
	int numInstances;

	if( ! ( r_drawelements->integer || rb.currentEntity == &rb.nullEnt ) || !de )
		return;

	numVerts = de->numVerts;
	numElems = de->numElems;
	firstVert = de->firstVert;
	firstElem = de->firstElem;
	numInstances = de->numInstances;

	if( numInstances ) {
		if( glConfig.ext.instanced_arrays ) {
			// the instance data is contained in vertex attributes
			qglDrawElementsInstancedARB( rb.primitive, numElems, GL_UNSIGNED_SHORT, 
				(GLvoid *)(firstElem * sizeof( elem_t )), numInstances );

			rb.stats.c_totalDraws++;
		} else if( glConfig.ext.draw_instanced ) {
			int i, numUInstances = 0;

			// manually update uniform values for instances for currently bound program,
			// respecting the MAX_GLSL_UNIFORM_INSTANCES limit
			for( i = 0; i < numInstances; i += numUInstances ) {
				numUInstances = min( numInstances - i, MAX_GLSL_UNIFORM_INSTANCES );

				RB_SetInstanceData( numUInstances, rb.drawInstances + i );

				qglDrawElementsInstancedARB( rb.primitive, numElems, GL_UNSIGNED_SHORT, 
					(GLvoid *)(firstElem * sizeof( elem_t )), numUInstances );

				rb.stats.c_totalDraws++;
			}
		} else {
			int i;

			// manually update uniform values for instances for currently bound program,
			// one by one
			for( i = 0; i < numInstances; i++ ) {
				RB_SetInstanceData( 1, rb.drawInstances + i );

				if( glConfig.ext.draw_range_elements ) {
					qglDrawRangeElementsEXT( rb.primitive, 
						firstVert, firstVert + numVerts - 1, numElems, 
						GL_UNSIGNED_SHORT, (GLvoid *)(firstElem * sizeof( elem_t )) );
				} else {
					qglDrawElements( rb.primitive, numElems, GL_UNSIGNED_SHORT,
						(GLvoid *)(firstElem * sizeof( elem_t )) );
				}

				rb.stats.c_totalDraws++;
			}
		}
	}
	else {
		numInstances = 1;

		if( glConfig.ext.draw_range_elements ) {
			qglDrawRangeElementsEXT( rb.primitive, 
				firstVert, firstVert + numVerts - 1, numElems, 
				GL_UNSIGNED_SHORT, (GLvoid *)(firstElem * sizeof( elem_t )) );
		} else {
			qglDrawElements( rb.primitive, numElems, GL_UNSIGNED_SHORT,
				(GLvoid *)(firstElem * sizeof( elem_t )) );
		}

		rb.stats.c_totalDraws++;
	}

	rb.stats.c_totalVerts += numVerts * numInstances;
	if( rb.primitive == GL_TRIANGLES ) {
		rb.stats.c_totalTris += numElems * numInstances / 3;
	}
}

/*
* RB_GetVertexAttribs
*/
vattribmask_t RB_GetVertexAttribs( void )
{
	return rb.currentVAttribs;
}

/*
* RB_DrawElements_
*/
static void RB_DrawElements_( void )
{
	if ( !rb.drawElements.numVerts || !rb.drawElements.numElems ) {
		return;
	}

	assert( rb.currentShader != NULL );

	RB_EnableVertexAttribs();

	if( rb.triangleOutlines ) {
		RB_DrawOutlinedElements();
	} else {
		RB_DrawShadedElements();
	}
}

/*
* RB_DrawElements
*/
void RB_DrawElements( int firstVert, int numVerts, int firstElem, int numElems,
	int firstShadowVert, int numShadowVerts, int firstShadowElem, int numShadowElems )
{
	rb.currentVAttribs &= ~VATTRIB_INSTANCES_BITS;

	rb.drawElements.numVerts = numVerts;
	rb.drawElements.numElems = numElems;
	rb.drawElements.firstVert = firstVert;
	rb.drawElements.firstElem = firstElem;
	rb.drawElements.numInstances = 0;

	rb.drawShadowElements.numVerts = numShadowVerts;
	rb.drawShadowElements.numElems = numShadowElems;
	rb.drawShadowElements.firstVert = firstShadowVert;
	rb.drawShadowElements.firstElem = firstShadowElem;
	rb.drawShadowElements.numInstances = 0;

	RB_DrawElements_();
}

/*
* RB_DrawElementsInstanced
*
* Draws <numInstances> instances of elements
*/
void RB_DrawElementsInstanced( int firstVert, int numVerts, int firstElem, int numElems,
	int firstShadowVert, int numShadowVerts, int firstShadowElem, int numShadowElems,
	int numInstances, instancePoint_t *instances )
{
	if( !numInstances ) {
		return;
	}

	rb.drawElements.numVerts = numVerts;
	rb.drawElements.numElems = numElems;
	rb.drawElements.firstVert = firstVert;
	rb.drawElements.firstElem = firstElem;
	rb.drawElements.numInstances = 0;

	rb.drawShadowElements.numVerts = numShadowVerts;
	rb.drawShadowElements.numElems = numShadowElems;
	rb.drawShadowElements.firstVert = firstShadowVert;
	rb.drawShadowElements.firstElem = firstShadowElem;
	rb.drawShadowElements.numInstances = 0;

	// check for vertex-attrib-divisor style instancing
	if( glConfig.ext.instanced_arrays ) {
		// upload instances
		if( rb.currentVBOId < RB_VBO_NONE ) {
			rb.currentVAttribs |= VATTRIB_INSTANCES_BITS;

			// FIXME: this is nasty!
			while( numInstances > MAX_STREAM_VBO_INSTANCES ) {
				R_UploadVBOInstancesData( rb.currentVBO, 0, MAX_STREAM_VBO_INSTANCES, instances );

				rb.drawElements.numInstances = MAX_STREAM_VBO_INSTANCES;
				rb.drawShadowElements.numInstances = MAX_STREAM_VBO_INSTANCES;
				RB_DrawElements_();

				instances += MAX_STREAM_VBO_INSTANCES;
				numInstances -= MAX_STREAM_VBO_INSTANCES;
			}

			if( !numInstances ) {
				return;
			}

			R_UploadVBOInstancesData( rb.currentVBO, 0, numInstances, instances );
		} else if( rb.currentVBO->instancesOffset ) {
			// static VBO's must come with their own set of instance data
			rb.currentVAttribs |= VATTRIB_INSTANCES_BITS;
		}
	}

	if( !( rb.currentVAttribs & VATTRIB_INSTANCES_BITS ) ) {
		// can't use instanced arrays so we'll have to manually update
		// the uniform state in between draw calls
		if( rb.maxDrawInstances < numInstances ) {
			if( rb.drawInstances ) {
				RB_Free( rb.drawInstances );
			}
			rb.drawInstances = RB_Alloc( numInstances * sizeof( *rb.drawInstances ) );
			rb.maxDrawInstances = numInstances;
		}
		memcpy( rb.drawInstances, instances, numInstances * sizeof( *instances ) );
	}

	rb.drawElements.numInstances = numInstances;
	rb.drawShadowElements.numInstances = numInstances;
	RB_DrawElements_();
}

/*
* RB_SetCamera
*/
void RB_SetCamera( const vec3_t cameraOrigin, const mat3_t cameraAxis )
{
	VectorCopy( cameraOrigin, rb.cameraOrigin );
	Matrix3_Copy( cameraAxis, rb.cameraAxis );
}

/*
* RB_SetRenderFlags
*/
void RB_SetRenderFlags( int flags )
{
	rb.renderFlags = flags;
}

/*
* RB_Finish
*/
void RB_Finish( void )
{
	qglFinish();
}
	
/*
* RB_Flush
*/
void RB_Flush( void )
{
	qglFlush();
}

/*
* RB_EnableTriangleOutlines
*
* Returns triangle outlines state before the call
*/
bool RB_EnableTriangleOutlines( bool enable )
{
	bool oldVal = rb.triangleOutlines;

	if( rb.triangleOutlines != enable ) {
		rb.triangleOutlines = enable;

		// OpenGL ES systems don't support glPolygonMode
#ifndef GL_ES_VERSION_2_0
		if( enable ) {
			RB_SetShaderStateMask( 0, GLSTATE_NO_DEPTH_TEST );
			qglPolygonMode( GL_FRONT_AND_BACK, GL_LINE );
		}
		else {
			RB_SetShaderStateMask( ~0, 0 );
			qglPolygonMode( GL_FRONT_AND_BACK, GL_FILL );
		}
#endif
	}

	return oldVal;
}

/*
* RB_ScissorForBounds
*/
bool RB_ScissorForBounds( vec3_t bbox[8], int *x, int *y, int *w, int *h )
{
	int i;
	int ix1, iy1, ix2, iy2;
	float x1, y1, x2, y2;
	vec4_t corner = { 0, 0, 0, 1 }, proj = { 0, 0, 0, 1 }, v = { 0, 0, 0, 1 };
	mat4_t cameraProjectionMatrix;

	Matrix4_Multiply( rb.projectionMatrix, rb.cameraMatrix, cameraProjectionMatrix );

	x1 = y1 = 999999;
	x2 = y2 = -999999;
	for( i = 0; i < 8; i++ )
	{
		// compute and rotate the full bounding box
		VectorCopy( bbox[i], corner );

		Matrix4_Multiply_Vector( cameraProjectionMatrix, corner, proj );

		if( proj[3] ) {
			v[0] = ( proj[0] / proj[3] + 1.0f ) * 0.5f * rb.gl.viewport[2];
			v[1] = ( proj[1] / proj[3] + 1.0f ) * 0.5f * rb.gl.viewport[3];
			v[2] = ( proj[2] / proj[3] + 1.0f ) * 0.5f; // [-1..1] -> [0..1]
		} else {
			v[0] = 999999.0f;
			v[1] = 999999.0f;
			v[2] = 999999.0f;
		}

		x1 = min( x1, v[0] ); y1 = min( y1, v[1] );
		x2 = max( x2, v[0] ); y2 = max( y2, v[1] );
	}

	ix1 = max( x1 - 1.0f, 0 ); ix2 = min( x2 + 1.0f, rb.gl.viewport[2] );
	if( ix1 >= ix2 )
		return false; // FIXME

	iy1 = max( y1 - 1.0f, 0 ); iy2 = min( y2 + 1.0f, rb.gl.viewport[3] );
	if( iy1 >= iy2 )
		return false; // FIXME

	*x = ix1;
	*y = rb.gl.viewport[3] - iy2;
	*w = ix2 - ix1;
	*h = iy2 - iy1;

	return true;
}
