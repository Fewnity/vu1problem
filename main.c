/*
# _____     ___ ____     ___ ____
#  ____|   |    ____|   |        | |____|
# |     ___|   |____ ___|    ____| |    \    PS2DEV Open Source Project.
#-----------------------------------------------------------------------
# (c) 2020 h4570 Sandro Sobczyński <sandro.sobczynski@gmail.com>
# Licenced under Academic Free License version 2.0
# Review ps2sdk README & LICENSE files for further details.
# VU1 and libpacket2 showcase.
*/

#include <kernel.h>
#include <malloc.h>
#include <tamtypes.h>
#include <gs_psm.h>
#include <dma.h>
#include <stdio.h>
#include <packet2.h>
#include <packet2_utils.h>
#include <graph.h>
#include <draw.h>
#include "zbyszek.c"
#include "mesh_data.c"
#include <unistd.h>
#include <time.h>
#include <gsKit.h>
#include <gsInline.h>
#include <dmaKit.h>
#include <stdlib.h>

// ---
// Variables declared as global for tutorial only!
// ---

/** Data of our texture (24bit, RGB8) */
extern unsigned char zbyszek[];

static const u64 BLACK_RGBAQ = GS_SETREG_RGBAQ(0x80, 0x80, 0x00, 0x80, 0x00);
/**
 * Data of VU1 micro program (draw_3D.vcl/vsm).
 * How we can use it:
 * 1. Upload program to VU1.
 * 2. Send calculated local_screen matrix once per mesh (3D object)
 * 3. Set buffers size. (double-buffering described below)
 * 4. Send packet with: lod, clut, tex buffer, scale vector, rgba, verts and sts.
 * What this program is doing?
 * 1. Load local_screen.
 * 2. Zero clipping flag.
 * 3. Set current buffer start address from TOP register (xtop command)
 *      To use pararelism, we set two buffers in the VU1. It means, that when
 *      VU1 is working with one verts packet, we can load second one into another buffer.
 *      xtop command is automatically switching buffers. I think that AAA games used
 *      quad buffers (TOP+TOPS) which can give best performance and no VIF_FLUSH should be needed.
 * 4. Load rest of data.
 * 5. Prepare GIF tag.
 * 6. For every vertex: transform, clip, scale, perspective divide.
 * 7. Send it to GS via XGKICK command.
 */
extern u32 VU1Draw3D_CodeStart __attribute__((section(".vudata")));
extern u32 VU1Draw3D_CodeEnd __attribute__((section(".vudata")));

VECTOR object_rotation = {0.00f, 0.00f, 0.00f, 1.00f};
VECTOR camera_position = {40.00f, 35.00f, 150.00f, 1.00f};
VECTOR camera_rotation = {0.00f, 0.00f, 0.00f, 1.00f};
MATRIX local_world, world_view, view_screen, local_screen;

framebuffer_t frame;
zbuffer_t z;
texbuffer_t texbuff;

/**
 * Packets for sending VU data
 * Each packet will have:
 * a) View/Projection matrix (calculated every frame)
 * b) Cube data (prim,lod,vertices,sts,...) added from zbyszek_packet.
 */
packet2_t *vif_packets[2] __attribute__((aligned(64)));
packet2_t *curr_vif_packet;

/** Cube data */
packet2_t *zbyszek_packet;

u64 *vif_packets2[2] __attribute__((aligned(64)));
u64 *curr_vif_packet2;
u64 *zbyszek_packet2;

u8 context = 0;

/** Set GS primitive type of drawing. */
prim_t prim;

/**
 * Color look up table.
 * Needed for texture.
 */
clutbuffer_t clut;

/**
 * Level of details.
 * Needed for texture.
 */
lod_t lod;

/**
 * Helper arrays.
 * Needed for calculations.
 */
VECTOR *c_verts __attribute__((aligned(128))), *c_sts __attribute__((aligned(128)));

#define BATCH 69

void vifSendPacket(void *packet, u32 vif_channel)
{
	dmaKit_wait(DMA_CHANNEL_GIF, 0);
	dmaKit_wait(vif_channel, 0);
	FlushCache(0);
	dmaKit_send_chain(vif_channel, (void *)((u32)packet & 0x0FFFFFFF), 0);
}

void *vifCreatePacket(u32 size)
{
	return memalign(128, size * 16);
}

void vifDestroyPacket(void *packet)
{
	free(packet);
}

/** Calculate packet for cube data */
void calculate_cube(texbuffer_t *t_texbuff)
{
	packet2_add_float(zbyszek_packet, 2048.0F);					  // scale
	packet2_add_float(zbyszek_packet, 2048.0F);					  // scale
	packet2_add_float(zbyszek_packet, ((float)0xFFFFFF) / 32.0F); // scale
	packet2_add_s32(zbyszek_packet, BATCH);						  // vertex count
	packet2_utils_gif_add_set(zbyszek_packet, 1);
	packet2_utils_gs_add_lod(zbyszek_packet, &lod);
	packet2_utils_gs_add_texbuff_clut(zbyszek_packet, t_texbuff, &clut);
	packet2_utils_gs_add_prim_giftag(zbyszek_packet, &prim, BATCH, DRAW_STQ2_REGLIST, 3, 0);
	u8 j = 0; // RGBA
	for (j = 0; j < 4; j++)
		packet2_add_u32(zbyszek_packet, 128);
}

#define VIF_CODE(_immediate, _num, _cmd, _irq) ((u32)(_immediate) | ((u32)(_num) << 16) | ((u32)(_cmd) << 24) | ((u32)(_irq) << 31))
#define UNPACK_V4_32 0x0C

#define VIF_NOP 0
#define VIF_STCYCL 1
#define VIF_FLUSH 17
#define VIF_FLUSHA 19
#define VIF_MSCAL 20
#define VIF_MSCALF 21
#define VIF_DIRECT 80

inline u64 *vu_add_unpack_data(u64 *p_data, u32 t_dest_address, void *t_data, u32 t_size, u8 t_use_top)
{
	*p_data++ = DMA_TAG(t_size, 0, DMA_REF, 0, t_data, 0);
	*p_data++ = (VIF_CODE(0x0101 | (0 << 8), 0, VIF_STCYCL, 0) | (u64)
																		 VIF_CODE(t_dest_address | ((u32)1 << 14) | ((u32)t_use_top << 15), ((t_size == 256) ? 0 : t_size), UNPACK_V4_32 | ((u32)0 << 4) | 0x60, 0)
																	 << 32);

	return p_data;
}

void packet2_utils_vu_add_start_program2(packet2_t *packet2, u32 addr)
{
	packet2_chain_open_cnt(packet2, 0, 0, 0);
	packet2_vif_flush(packet2, 0);
	packet2_vif_mscalf(packet2, addr, 0);
	packet2_chain_close_tag(packet2);
}

/** Calculate cube position and add packet with cube data */
void draw_cube(VECTOR t_object_position, texbuffer_t *t_texbuff)
{
	create_local_world(local_world, t_object_position, object_rotation);
	create_world_view(world_view, camera_position, camera_rotation);
	create_local_screen(local_screen, local_world, world_view, view_screen);

	int verticesToDraw = faces_count;
	int verticesDrawn = 0;
	while (verticesToDraw > 0)
	{
		dma_channel_wait(DMA_CHANNEL_VIF1, 0);

		int count = BATCH;
		if (verticesToDraw < BATCH)
		{
			count = verticesToDraw;
		}

		curr_vif_packet2 = vif_packets2[context];
		memset(curr_vif_packet2, 0, 16 * 6);

		*curr_vif_packet2++ = DMA_TAG(0, 0, DMA_CNT, 0, 0, 0);
		*curr_vif_packet2++ = ((VIF_CODE(0, 0, VIF_FLUSH, 0) | (u64)VIF_CODE(0, 0, VIF_NOP, 0) << 32));

		// Add matrix at the beggining of VU mem (skip TOP)
		curr_vif_packet2 = vu_add_unpack_data(curr_vif_packet2, 0, &local_screen, 8, 0);

		u32 vif_added_bytes = 0; // zero because now we will use TOP register (double buffer)
								 // we don't wan't to unpack at 8 + beggining of buffer, but at
								 // the beggining of the buffer

		// Merge packets

		int size = packet2_get_qw_count(zbyszek_packet);
		curr_vif_packet2 = vu_add_unpack_data(curr_vif_packet2, vif_added_bytes, zbyszek_packet->base, size, 1);
		vif_added_bytes += size;

		// Add vertices
		curr_vif_packet2 = vu_add_unpack_data(curr_vif_packet2, vif_added_bytes, c_verts + verticesDrawn, BATCH, 1);
		vif_added_bytes += BATCH; // one VECTOR is size of qword

		// Add sts
		curr_vif_packet2 = vu_add_unpack_data(curr_vif_packet2, vif_added_bytes, c_sts + verticesDrawn, BATCH, 1);
		vif_added_bytes += BATCH;

		*curr_vif_packet2++ = DMA_TAG(0, 0, DMA_CNT, 0, 0, 0);
		*curr_vif_packet2++ = ((VIF_CODE(0, 0, VIF_FLUSH, 0) | (u64)VIF_CODE(0, 0, VIF_MSCALF, 0) << 32));

		*curr_vif_packet2++ = DMA_TAG(0, 0, DMA_END, 0, 0, 0);
		*curr_vif_packet2++ = (VIF_CODE(0, 0, VIF_NOP, 0) | (u64)VIF_CODE(0, 0, VIF_NOP, 0) << 32);

		asm volatile("nop" ::: "memory");

		vifSendPacket(vif_packets2[context], DMA_CHANNEL_VIF1);

		//  Switch packet, so we can proceed during DMA transfer
		verticesToDraw -= count;
		verticesDrawn += count;
		context = !context;
	}
}

/** Some initialization of GS and VRAM allocation */
void init_gs(framebuffer_t *t_frame, zbuffer_t *t_z, texbuffer_t *t_texbuff)
{
	// Define a 32-bit 640x512 framebuffer.
	t_frame->width = 640;
	t_frame->height = 512;
	t_frame->mask = 0;
	t_frame->psm = GS_PSM_32;
	t_frame->address = graph_vram_allocate(t_frame->width, t_frame->height, t_frame->psm, GRAPH_ALIGN_PAGE);

	// Enable the zbuffer.
	t_z->enable = DRAW_ENABLE;
	t_z->mask = 0;
	t_z->method = ZTEST_METHOD_GREATER_EQUAL;
	t_z->zsm = GS_ZBUF_32;
	t_z->address = graph_vram_allocate(t_frame->width, t_frame->height, t_z->zsm, GRAPH_ALIGN_PAGE);

	// Allocate some vram for the texture buffer
	t_texbuff->width = 128;
	t_texbuff->psm = GS_PSM_24;
	t_texbuff->address = graph_vram_allocate(128, 128, GS_PSM_24, GRAPH_ALIGN_BLOCK);

	// Initialize the screen and tie the first framebuffer to the read circuits.
	graph_initialize(t_frame->address, t_frame->width, t_frame->height, t_frame->psm, 0, 0);
}

/** Some initialization of GS 2 */
void init_drawing_environment(framebuffer_t *t_frame, zbuffer_t *t_z)
{
	packet2_t *packet2 = packet2_create(20, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);

	// This will setup a default drawing environment.
	packet2_update(packet2, draw_setup_environment(packet2->next, 0, t_frame, t_z));

	// Now reset the primitive origin to 2048-width/2,2048-height/2.
	packet2_update(packet2, draw_primitive_xyoffset(packet2->next, 0, (2048 - 320), (2048 - 256)));

	// Finish setting up the environment.
	packet2_update(packet2, draw_finish(packet2->next));

	// Now send the packet, no need to wait since it's the first.
	FlushCache(0);
	dma_channel_send_packet2(packet2, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();

	packet2_free(packet2);
}

/** Send texture data to GS. */
void send_texture(texbuffer_t *texbuf)
{
	packet2_t *packet2 = packet2_create(50, P2_TYPE_NORMAL, P2_MODE_CHAIN, 0);
	packet2_update(packet2, draw_texture_transfer(packet2->next, zbyszek, 128, 128, GS_PSM_24, texbuf->address, texbuf->width));
	packet2_update(packet2, draw_texture_flush(packet2->next));
	FlushCache(0);
	dma_channel_send_packet2(packet2, DMA_CHANNEL_GIF, 1);
	dma_wait_fast();
	packet2_free(packet2);
}

qword_t *draw_packet = NULL;

/** Send packet which will clear our screen. */
void clear_screen(framebuffer_t *tframe, zbuffer_t *tz)
{
	packet2_t *clear = packet2_create(35, P2_TYPE_NORMAL, P2_MODE_NORMAL, 0);

	// Clear framebuffer but don't update zbuffer.
	packet2_update(clear, draw_disable_tests(clear->next, 0, tz));
	packet2_update(clear, draw_clear(clear->next, 0, 2048.0f - 320.0f, 2048.0f - 256.0f, tframe->width, tframe->height, 0x40, 0x40, 0x40));
	packet2_update(clear, draw_enable_tests(clear->next, 0, tz));
	packet2_update(clear, draw_finish(clear->next));

	// Now send our current dma chain.
	dma_wait_fast();
	FlushCache(0);
	dma_channel_send_packet2(clear, DMA_CHANNEL_GIF, 1);

	packet2_free(clear);

	// Wait for scene to finish drawing
	draw_wait_finish();
}

void set_lod_clut_prim_tex_buff(texbuffer_t *t_texbuff)
{
	lod.calculation = LOD_USE_K;
	lod.max_level = 0;
	lod.mag_filter = LOD_MAG_NEAREST;
	lod.min_filter = LOD_MIN_NEAREST;
	lod.l = 0;
	lod.k = 0;

	clut.storage_mode = CLUT_STORAGE_MODE1;
	clut.start = 0;
	clut.psm = 0;
	clut.load_method = CLUT_NO_LOAD;
	clut.address = 0;

	// Define the triangle primitive we want to use.
	prim.type = PRIM_TRIANGLE;
	prim.shading = PRIM_SHADE_GOURAUD;
	prim.mapping = DRAW_ENABLE;
	prim.fogging = DRAW_DISABLE;
	prim.blending = DRAW_ENABLE;
	prim.antialiasing = DRAW_DISABLE;
	prim.mapping_type = PRIM_MAP_ST;
	prim.colorfix = PRIM_UNFIXED;

	t_texbuff->info.width = draw_log2(128);
	t_texbuff->info.height = draw_log2(128);
	t_texbuff->info.components = TEXTURE_COMPONENTS_RGB;
	t_texbuff->info.function = TEXTURE_FUNCTION_DECAL;
}

void render(framebuffer_t *t_frame, zbuffer_t *t_z, texbuffer_t *t_texbuff)
{
	int i, j;

	set_lod_clut_prim_tex_buff(t_texbuff);

	/**
	 * Allocate some space for object position calculating.
	 * c_ prefix = calc_
	 */
	c_verts = (VECTOR *)memalign(128, sizeof(VECTOR) * faces_count);
	c_sts = (VECTOR *)memalign(128, sizeof(VECTOR) * faces_count);

	VECTOR c_zbyszek_position;

	for (i = 0; i < faces_count; i++)
	{
		c_verts[i][0] = vertices[faces[i]][0];
		c_verts[i][1] = vertices[faces[i]][1];
		c_verts[i][2] = vertices[faces[i]][2];
		c_verts[i][3] = vertices[faces[i]][3];

		// The teapot does not have uv, so give random uv
		c_sts[i][0] = (rand() % 100) / 100.0F;
		c_sts[i][1] = (rand() % 100) / 100.0F;
		c_sts[i][2] = 1;
		c_sts[i][3] = 0;
	}

	// Create the view_screen matrix.
	create_view_screen(view_screen, graph_aspect_ratio(), -3.00f, 3.00f, -3.00f, 3.00f, 1.00f, 2000.00f);
	calculate_cube(t_texbuff);

	// The main loop...
	for (;;)
	{
		// Spin the cube a bit.
		object_rotation[0] += 0.008f;
		while (object_rotation[0] > 3.14f)
		{
			object_rotation[0] -= 6.28f;
		}
		object_rotation[1] += 0.012f;
		while (object_rotation[1] > 3.14f)
		{
			object_rotation[1] -= 6.28f;
		}

		// camera_position[2] += .5F;
		// camera_rotation[2] += 0.002f;
		// if (camera_position[2] >= 400.0F)
		// {
		// 	camera_position[2] = 40.0F;
		// 	camera_rotation[2] = 0.00f;
		// }

		clear_screen(t_frame, t_z);

		for (i = 0; i < 3; i++)
		{
			c_zbyszek_position[0] = i * 40.0F;
			for (j = 0; j < 3; j++)
			{
				c_zbyszek_position[1] = j * 40.0F;
				for (int z = 0; z < 2; z++)
				{
					c_zbyszek_position[2] = -z * 40.0F;
					draw_cube(c_zbyszek_position, t_texbuff);
				}
			}
		}

		graph_wait_vsync();
	}
}

void vu1_set_double_buffer_settings()
{
	packet2_t *packet2 = packet2_create(1, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_utils_vu_add_double_buffer(packet2, 8, 496);
	packet2_utils_vu_add_end_tag(packet2);
	FlushCache(0);
	dma_channel_send_packet2(packet2, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(packet2);
}

void vu1_upload_micro_program()
{
	u32 packet_size =
		packet2_utils_get_packet_size_for_program(&VU1Draw3D_CodeStart, &VU1Draw3D_CodeEnd) + 1; // + 1 for end tag
	packet2_t *packet2 = packet2_create(packet_size, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	packet2_vif_add_micro_program(packet2, 0, &VU1Draw3D_CodeStart, &VU1Draw3D_CodeEnd);
	packet2_utils_vu_add_end_tag(packet2);
	FlushCache(0);
	dma_channel_send_packet2(packet2, DMA_CHANNEL_VIF1, 1);
	dma_channel_wait(DMA_CHANNEL_VIF1, 0);
	packet2_free(packet2);
}

int main(int argc, char *argv[])
{
	// Init DMA channels.
	dma_channel_initialize(DMA_CHANNEL_GIF, NULL, 0);
	dma_channel_initialize(DMA_CHANNEL_VIF1, NULL, 0);
	dma_channel_fast_waits(DMA_CHANNEL_GIF);
	dma_channel_fast_waits(DMA_CHANNEL_VIF1);

	// Initialize vif packets
	zbyszek_packet = packet2_create(10, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	vif_packets[0] = packet2_create(50, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);
	vif_packets[1] = packet2_create(50, P2_TYPE_NORMAL, P2_MODE_CHAIN, 1);

	zbyszek_packet2 = vifCreatePacket(6);
	vif_packets2[0] = vifCreatePacket(9);
	vif_packets2[1] = vifCreatePacket(9);

	vu1_upload_micro_program();
	vu1_set_double_buffer_settings();

	//  Init the GS, framebuffer, zbuffer, and texture buffer.
	init_gs(&frame, &z, &texbuff);

	// Init the drawing environment and framebuffer.
	init_drawing_environment(&frame, &z);

	// Load the texture into vram.
	send_texture(&texbuff);

	// Render textured cube
	render(&frame, &z, &texbuff);

	packet2_free(vif_packets[0]);
	packet2_free(vif_packets[1]);
	packet2_free(zbyszek_packet);

	// Sleep
	SleepThread();

	// End program.
	return 0;
}
