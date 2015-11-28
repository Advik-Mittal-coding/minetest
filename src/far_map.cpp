/*
Minetest
Copyright (C) 2015 celeron55, Perttu Ahola <celeron55@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "far_map.h"
#include "constants.h"
#include "settings.h"
#include "mapblock_mesh.h" // MeshCollector
#include "mesh.h" // translateMesh
#include "util/numeric.h" // getContainerPos
#include "client.h" // For use of Client's IGameDef interface
#include "profiler.h"
#include "irrlichttypes_extrabloated.h"
#include <IMaterialRenderer.h>

#define PP(x) "("<<(x).X<<","<<(x).Y<<","<<(x).Z<<")"

FarMapBlock::FarMapBlock(v3s16 p):
	p(p),
	mesh(NULL)
{
}

FarMapBlock::~FarMapBlock()
{
	if(mesh)
		mesh->drop();
}

void FarMapBlock::resize(v3s16 new_block_div)
{
	block_div = new_block_div;

	v3s16 area_size(FMP_SCALE, FMP_SCALE, FMP_SCALE);

	total_size = v3s16(
			area_size.X * block_div.X,
			area_size.Y * block_div.Y,
			area_size.Z * block_div.Z);

	size_t total_size_n = total_size.X * total_size.Y * total_size.Z;

	content.resize(total_size_n);
}

void FarMapBlock::updateCameraOffset(v3s16 camera_offset)
{
	if (!mesh) return;

	if (camera_offset != current_camera_offset) {
		translateMesh(mesh, intToFloat(current_camera_offset-camera_offset, BS));
		current_camera_offset = camera_offset;
	}
}

void FarMapBlock::resetCameraOffset(v3s16 camera_offset)
{
	current_camera_offset = v3s16(0, 0, 0);
	updateCameraOffset(camera_offset);
}

FarMapSector::FarMapSector(v2s16 p):
	p(p)
{
}

FarMapSector::~FarMapSector()
{
	for (std::map<s16, FarMapBlock*>::iterator i = blocks.begin();
			i != blocks.end(); i++) {
		delete i->second;
	}
}

FarMapBlock* FarMapSector::getOrCreateBlock(s16 y)
{
	std::map<s16, FarMapBlock*>::iterator i = blocks.find(y);
	if(i != blocks.end())
		return i->second;
	v3s16 p3d(p.X, y, p.Y);
	FarMapBlock *b = new FarMapBlock(p3d);
	blocks[y] = b;
	return b;
}

FarMapBlockMeshGenerateTask::FarMapBlockMeshGenerateTask(
		FarMap *far_map, const FarMapBlock &source_block_):
	far_map(far_map),
	source_block(source_block_),
	mesh(NULL)
{
	// We don't want to deal with whatever mesh the block is currently holding;
	// set it to NULL so that no destructor will drop it.
	source_block.mesh = NULL;
}

FarMapBlockMeshGenerateTask::~FarMapBlockMeshGenerateTask()
{
	if(mesh)
		mesh->drop();
}

static void add_face(MeshCollector *collector,
		const v3f base_pf, const FarMapNode &n,
		const v3s16 &p, s16 dir_x, s16 dir_y, s16 dir_z,
		const std::vector<FarMapNode> &data,
		const VoxelArea &data_area,
		const v3s16 &block_div,
		const FarMap *far_map)
{
	ITextureSource *tsrc = far_map->client->getTextureSource();
	//IShaderSource *ssrc = far_map->client->getShaderSource();
	//INodeDefManager *ndef = far_map->client->getNodeDefManager();

	static const u16 indices[] = {0,1,2,2,3,0};

	v3s16 dir(dir_x, dir_y, dir_z);
	v3s16 vertex_dirs[4];
	getNodeVertexDirs(dir, vertex_dirs);

	// This is the size of one FarMapNode (without BS being factored in)
	v3f scale(
		MAP_BLOCKSIZE / block_div.X,
		MAP_BLOCKSIZE / block_div.Y,
		MAP_BLOCKSIZE / block_div.Z
	);

	v3f pf = base_pf;
	pf += v3f(
		scale.X * p.X * BS,
		scale.Y * p.Y * BS,
		scale.Z * p.Z * BS
	);

	v3f vertex_pos[4];
	for(u16 i=0; i<4; i++)
	{
		vertex_pos[i] = v3f(
				BS/2*vertex_dirs[i].X,
				BS/2*vertex_dirs[i].Y,
				BS/2*vertex_dirs[i].Z
		);
		vertex_pos[i].X *= scale.X;
		vertex_pos[i].Y *= scale.Y;
		vertex_pos[i].Z *= scale.Z;
		vertex_pos[i] += pf;
	}

	v3f normal(dir.X, dir.Y, dir.Z);

	u8 alpha = 255;

	// As produced by getFaceLight (day | (night << 8))
	u16 light_encoded = (255) | (255<<8);
	// Light produced by the node itself
	u8 light_source = 0;

	// Some kind of texture coordinate aspect stretch thing
	f32 abs_scale = 1.0;
	if     (scale.X < 0.999 || scale.X > 1.001) abs_scale = scale.X;
	else if(scale.Y < 0.999 || scale.Y > 1.001) abs_scale = scale.Y;
	else if(scale.Z < 0.999 || scale.Z > 1.001) abs_scale = scale.Z;

	// Texture coordinates
	float x0 = 0.0;
	float y0 = 0.0;
	float w = 1.0;
	float h = 1.0;

	video::S3DVertex vertices[4];
	vertices[0] = video::S3DVertex(vertex_pos[0], normal,
			MapBlock_LightColor(alpha, light_encoded, light_source),
			core::vector2d<f32>(x0+w*abs_scale, y0+h));
	vertices[1] = video::S3DVertex(vertex_pos[1], normal,
			MapBlock_LightColor(alpha, light_encoded, light_source),
			core::vector2d<f32>(x0, y0+h));
	vertices[2] = video::S3DVertex(vertex_pos[2], normal,
			MapBlock_LightColor(alpha, light_encoded, light_source),
			core::vector2d<f32>(x0, y0));
	vertices[3] = video::S3DVertex(vertex_pos[3], normal,
			MapBlock_LightColor(alpha, light_encoded, light_source),
			core::vector2d<f32>(x0+w*abs_scale, y0));

	// TODO: Don't do this; use a special texture atlas
	//std::string tile_name = "default_grass.png";
	std::string tile_name = "unknown_node.png";

	TileSpec t;
	t.texture_id = tsrc->getTextureId(tile_name);
	t.texture = tsrc->getTexture(t.texture_id);
	t.alpha = alpha;
	t.material_type = TILE_MATERIAL_BASIC;
	t.material_flags &= ~MATERIAL_FLAG_BACKFACE_CULLING;

	infostream<<"FarMapBlockMeshGenerate: enable_shaders="
			<<(far_map->config_enable_shaders?"true":"false")<<std::endl;

	if (far_map->config_enable_shaders) {
		t.shader_id = far_map->farblock_shader_id;
		bool normalmap_present = false;
		t.flags_texture = tsrc->getShaderFlagsTexture(normalmap_present);
	}

	collector->append(t, vertices, 4, indices, 6);
}

static void extract_faces(MeshCollector *collector,
		const v3f base_pf,
		const std::vector<FarMapNode> &data,
		const VoxelArea &data_area, const VoxelArea &gen_area,
		const v3s16 &block_div,
		const FarMap *far_map,
		size_t *profiler_num_faces_added)
{
	// At least one extra node at each edge is required. This enables speed
	// optimization of lookups in this algorithm.
	assert(data_area.MinEdge.X <= gen_area.MinEdge.X - 1);
	assert(data_area.MinEdge.Y <= gen_area.MinEdge.Y - 1);
	assert(data_area.MinEdge.Z <= gen_area.MinEdge.Z - 1);
	assert(data_area.MaxEdge.X >= gen_area.MaxEdge.X + 1);
	assert(data_area.MaxEdge.Y >= gen_area.MaxEdge.Y + 1);
	assert(data_area.MaxEdge.Z >= gen_area.MaxEdge.Z + 1);

	INodeDefManager *ndef = far_map->client->getNodeDefManager();

	v3s16 data_extent = data_area.getExtent();

	v3s16 p000;
	for (p000.Y=gen_area.MinEdge.Y; p000.Y<=gen_area.MaxEdge.Y; p000.Y++)
	for (p000.X=gen_area.MinEdge.X; p000.X<=gen_area.MaxEdge.X; p000.X++)
	for (p000.Z=gen_area.MinEdge.Z; p000.Z<=gen_area.MaxEdge.Z; p000.Z++)
	{
		size_t i000 = data_area.index(p000);
		const FarMapNode &n000 = data[i000];
		const FarMapNode &n001 = data[data_area.added_z(data_extent, i000, 1)];
		const FarMapNode &n010 = data[data_area.added_y(data_extent, i000, 1)];
		const FarMapNode &n100 = data[data_area.added_x(data_extent, i000, 1)];
		const ContentFeatures &f000 = ndef->get(n000.id);
		const ContentFeatures &f001 = ndef->get(n001.id);
		const ContentFeatures &f010 = ndef->get(n010.id);
		const ContentFeatures &f100 = ndef->get(n100.id);
		int s000 = f000.solidness ?: f000.visual_solidness;
		int s001 = f001.solidness ?: f001.visual_solidness;
		int s010 = f010.solidness ?: f010.visual_solidness;
		int s100 = f100.solidness ?: f100.visual_solidness;

		// TODO: Somehow handle non-cubic things maybe
		// NOTE: Don't call 'continue' though; it would break this
		//       cube-collecting algorithm.
		/*if(f000.drawtype == NDT_){
		}*/

		if(s000 > s001){
			add_face(collector, base_pf, n000, p000,  0,0,1, data,
					data_area, block_div, far_map);
			(*profiler_num_faces_added)++;
		}
		else if(s000 < s001){
			v3s16 p001 = p000 + v3s16(0,0,1);
			add_face(collector, base_pf, n001, p001, 0,0,-1, data,
					data_area, block_div, far_map);
			(*profiler_num_faces_added)++;
		}
		if(s000 > s010){
			add_face(collector, base_pf, n000, p000,  0,1,0, data,
					data_area, block_div, far_map);
			(*profiler_num_faces_added)++;
		}
		else if(s000 < s010){
			v3s16 p010 = p000 + v3s16(0,1,0);
			add_face(collector, base_pf, n010, p010, 0,-1,0, data,
					data_area, block_div, far_map);
			(*profiler_num_faces_added)++;
		}
		if(s000 > s100){
			add_face(collector, base_pf, n000, p000,  1,0,0, data,
					data_area, block_div, far_map);
			(*profiler_num_faces_added)++;
		}
		else if(s000 < s100){
			v3s16 p100 = p000 + v3s16(1,0,0);
			add_face(collector, base_pf, n100, p100, -1,0,0, data,
					data_area, block_div, far_map);
			(*profiler_num_faces_added)++;
		}
	}
}

void FarMapBlockMeshGenerateTask::inThread()
{
	infostream<<"Generating FarMapBlock mesh for "
			<<PP(source_block.p)<<std::endl;

	ITextureSource *tsrc = far_map->client->getTextureSource();
	IShaderSource *ssrc = far_map->client->getShaderSource();
	//INodeDefManager *ndef = far_map->client->getNodeDefManager();

	MeshCollector collector;

	v3s16 dp0(0, 0, 0);
	v3s16 dp1 = dp0 + source_block.total_size - v3s16(1,1,1); // Inclusive
	VoxelArea data_area(dp0, dp1);
	VoxelArea gen_area = data_area;
	// TODO: Extend source block content so that this doesn't need to be done
	gen_area.MinEdge += v3s16(1,1,1);
	gen_area.MaxEdge -= v3s16(1,1,1);

	v3f base_pf = v3f(source_block.p.X, source_block.p.Y, source_block.p.Z)
			* MAP_BLOCKSIZE * FMP_SCALE * BS;

	size_t profiler_num_faces_added = 0;

	extract_faces(&collector, base_pf, source_block.content, data_area,
			gen_area, source_block.block_div, far_map,
			&profiler_num_faces_added);

	g_profiler->avg("Far: num faces per mesh", profiler_num_faces_added);
	g_profiler->add("Far: num meshes generated", 1);
	

	// TODO

	// Test
	for (size_t i0=0; i0<5; i0++)
	{
		FarMapNode n000;
		n000.id = 5;
		n000.light = (15) | (15<<4);

		v3s16 p000(
			source_block.block_div.X * FMP_SCALE / 2,
			source_block.block_div.Y * FMP_SCALE / 5 * i0,
			source_block.block_div.Z * FMP_SCALE / 2
		);

		add_face(&collector, base_pf, n000, p000,  0,0,1, source_block.content,
				data_area, source_block.block_div, far_map);

#if 0
		const u16 indices[] = {0,1,2,2,3,0};

		v3s16 dir(0, 0, 1);
		v3s16 vertex_dirs[4];
		getNodeVertexDirs(dir, vertex_dirs);

		v3f scale(
			MAP_BLOCKSIZE / source_block.block_div.X,
			MAP_BLOCKSIZE / source_block.block_div.Y,
			MAP_BLOCKSIZE / source_block.block_div.Z
		);

		v3f pf = v3f(source_block.p.X, source_block.p.Y, source_block.p.Z)
				* MAP_BLOCKSIZE * FMP_SCALE * BS;
		pf += v3f(0.5, 0.0, 0.5) * MAP_BLOCKSIZE * FMP_SCALE * BS;
		pf.Y += 0.2 * MAP_BLOCKSIZE * FMP_SCALE * BS * i0;

		v3f vertex_pos[4];
		for(u16 i=0; i<4; i++)
		{
			vertex_pos[i] = v3f(
					BS/2*vertex_dirs[i].X,
					BS/2*vertex_dirs[i].Y,
					BS/2*vertex_dirs[i].Z
			);
			vertex_pos[i].X *= scale.X;
			vertex_pos[i].Y *= scale.Y;
			vertex_pos[i].Z *= scale.Z;
			vertex_pos[i] += pf;
		}

		v3f normal(dir.X, dir.Y, dir.Z);

		u8 alpha = 255;

		// As produced by getFaceLight (day | (night << 8))
		u16 light_encoded = (255) | (255<<8);
		// Light produced by the node itself
		u8 light_source = 0;

		// Some kind of texture coordinate aspect stretch thing
		f32 abs_scale = 1.0;
		if     (scale.X < 0.999 || scale.X > 1.001) abs_scale = scale.X;
		else if(scale.Y < 0.999 || scale.Y > 1.001) abs_scale = scale.Y;
		else if(scale.Z < 0.999 || scale.Z > 1.001) abs_scale = scale.Z;

		// Texture coordinates
		float x0 = 0.0;
		float y0 = 0.0;
		float w = 1.0;
		float h = 1.0;

		video::S3DVertex vertices[4];
		vertices[0] = video::S3DVertex(vertex_pos[0], normal,
				MapBlock_LightColor(alpha, light_encoded, light_source),
				core::vector2d<f32>(x0+w*abs_scale, y0+h));
		vertices[1] = video::S3DVertex(vertex_pos[1], normal,
				MapBlock_LightColor(alpha, light_encoded, light_source),
				core::vector2d<f32>(x0, y0+h));
		vertices[2] = video::S3DVertex(vertex_pos[2], normal,
				MapBlock_LightColor(alpha, light_encoded, light_source),
				core::vector2d<f32>(x0, y0));
		vertices[3] = video::S3DVertex(vertex_pos[3], normal,
				MapBlock_LightColor(alpha, light_encoded, light_source),
				core::vector2d<f32>(x0+w*abs_scale, y0));

		// TODO: Don't do this; use a special texture atlas
		//std::string tile_name = "default_grass.png";
		std::string tile_name = "unknown_node.png";

		TileSpec t;
		t.texture_id = tsrc->getTextureId(tile_name);
		t.texture = tsrc->getTexture(t.texture_id);
		t.alpha = alpha;
		t.material_type = TILE_MATERIAL_BASIC;
		t.material_flags &= ~MATERIAL_FLAG_BACKFACE_CULLING;

		infostream<<"FarMapBlockMeshGenerate: enable_shaders="
				<<(far_map->config_enable_shaders?"true":"false")<<std::endl;

		if (far_map->config_enable_shaders) {
			t.shader_id = far_map->farblock_shader_id;
			bool normalmap_present = false;
			t.flags_texture = tsrc->getShaderFlagsTexture(normalmap_present);
		}

		collector.append(t, vertices, 4, indices, 6);
#endif
	}
	
	/*
		Convert MeshCollector to SMesh
	*/

	assert(mesh == NULL);
	mesh = new scene::SMesh();

	for(u32 i = 0; i < collector.prebuffers.size(); i++)
	{
		PreMeshBuffer &p = collector.prebuffers[i];

		for(u32 j = 0; j < p.vertices.size(); j++)
		{
			video::S3DVertexTangents *vertex = &p.vertices[j];
			// Note applyFacesShading second parameter is precalculated sqrt
			// value for speed improvement
			// Skip it for lightsources and top faces.
			video::SColor &vc = vertex->Color;
			if (!vc.getBlue()) {
				if (vertex->Normal.Y < -0.5) {
					applyFacesShading (vc, 0.447213);
				} else if (vertex->Normal.X > 0.5) {
					applyFacesShading (vc, 0.670820);
				} else if (vertex->Normal.X < -0.5) {
					applyFacesShading (vc, 0.670820);
				} else if (vertex->Normal.Z > 0.5) {
					applyFacesShading (vc, 0.836660);
				} else if (vertex->Normal.Z < -0.5) {
					applyFacesShading (vc, 0.836660);
				}
			}
			if(!far_map->config_enable_shaders)
			{
				// - Classic lighting (shaders handle this by themselves)
				// Set initial real color and store for later updates
				u8 day = vc.getRed();
				u8 night = vc.getGreen();
				finalColorBlend(vc, day, night, 1000);
			}
		}

		// Create material
		video::SMaterial material;
		material.setFlag(video::EMF_LIGHTING, false);
		material.setFlag(video::EMF_BACK_FACE_CULLING, true);
		material.setFlag(video::EMF_BILINEAR_FILTER, false);
		material.setFlag(video::EMF_FOG_ENABLE, true);
		material.setTexture(0, p.tile.texture);

		// TODO: A special texture atlas needs to be generated to be used here

		if (far_map->config_enable_shaders) {
			material.MaterialType = ssrc->getShaderInfo(p.tile.shader_id).material;
			p.tile.applyMaterialOptionsWithShaders(material);
			if (p.tile.normal_texture) {
				material.setTexture(1, p.tile.normal_texture);
			}
			material.setTexture(2, p.tile.flags_texture);
		} else {
			p.tile.applyMaterialOptions(material);
		}

		// Create meshbuffer
		scene::SMeshBufferTangents *buf = new scene::SMeshBufferTangents();
		// Set material
		buf->Material = material;
		// Add to mesh
		mesh->addMeshBuffer(buf);
		// Mesh grabbed it
		buf->drop();
		buf->append(&p.vertices[0], p.vertices.size(),
			&p.indices[0], p.indices.size());
	}

	if (far_map->config_enable_shaders) {
		scene::IMeshManipulator* meshmanip =
				far_map->client->getSceneManager()->getMeshManipulator();
		meshmanip->recalculateTangents(mesh, true, false, false);
	}
}

void FarMapBlockMeshGenerateTask::sync()
{
	if(mesh){
		far_map->insertGeneratedBlockMesh(source_block.p, mesh);
		mesh->drop();
		mesh = NULL;
	} else {
		infostream<<"No FarMapBlock mesh result for "
				<<PP(source_block.p)<<std::endl;
	}
}

FarMapWorkerThread::~FarMapWorkerThread()
{
	verbosestream<<"FarMapWorkerThread: Deleting remaining tasks (in)"<<std::endl;
	for(;;){
		try {
			FarMapTask *t = m_queue_in.pop_front(0);
			delete t;
		} catch(ItemNotFoundException &e){
			break;
		}
	}
	verbosestream<<"FarMapWorkerThread: Deleting remaining tasks (sync)"<<std::endl;
	for(;;){
		try {
			FarMapTask *t = m_queue_sync.pop_front(0);
			delete t;
		} catch(ItemNotFoundException &e){
			break;
		}
	}
}

void FarMapWorkerThread::addTask(FarMapTask *task)
{
	g_profiler->add("Far: tasks added", 1);

	s32 length = ++m_queue_in_length;
	g_profiler->avg("Far: task queue length (avg)", length);

	m_queue_in.push_back(task);
	deferUpdate();
}

void FarMapWorkerThread::sync()
{
	for(;;){
		try {
			FarMapTask *t = m_queue_sync.pop_front(0);
			infostream<<"FarMapWorkerThread: Running task in sync"<<std::endl;
			t->sync();
			delete t;
			g_profiler->add("Far: tasks finished", 1);
		} catch(ItemNotFoundException &e){
			break;
		}
	}
}

void FarMapWorkerThread::doUpdate()
{
	for(;;){
		try {
			s32 length = --m_queue_in_length;
			g_profiler->avg("Far: task queue length (avg)", length);

			FarMapTask *t = m_queue_in.pop_front(250);
			infostream<<"FarMapWorkerThread: Running task in thread"<<std::endl;
			t->inThread();
			m_queue_sync.push_back(t);
		} catch(ItemNotFoundException &e){
			break;
		}
	}
}

FarMap::FarMap(
		Client *client,
		scene::ISceneNode* parent,
		scene::ISceneManager* mgr,
		s32 id
):
	scene::ISceneNode(parent, mgr, id),
	client(client),
	farblock_shader_id(0)
{
	m_bounding_box = core::aabbox3d<f32>(-BS*1000000,-BS*1000000,-BS*1000000,
			BS*1000000,BS*1000000,BS*1000000);

	updateSettings();
	
	m_worker_thread.start();
}

FarMap::~FarMap()
{
	m_worker_thread.stop();
	m_worker_thread.wait();

	for (std::map<v2s16, FarMapSector*>::iterator i = m_sectors.begin();
			i != m_sectors.end(); i++) {
		delete i->second;
	}
}

FarMapSector* FarMap::getOrCreateSector(v2s16 p)
{
	std::map<v2s16, FarMapSector*>::iterator i = m_sectors.find(p);
	if(i != m_sectors.end())
		return i->second;
	FarMapSector *s = new FarMapSector(p);
	m_sectors[p] = s;
	return s;
}

FarMapBlock* FarMap::getOrCreateBlock(v3s16 p)
{
	v2s16 p2d(p.X, p.Z);
	FarMapSector *s = getOrCreateSector(p2d);
	return s->getOrCreateBlock(p.Y);
}

void FarMap::insertData(v3s16 area_offset_mapblocks, v3s16 area_size_mapblocks,
		v3s16 block_div,
		const std::vector<u16> &node_ids, const std::vector<u8> &lights)
{
	infostream<<"FarMap::insertData: "
			<<"area_offset_mapblocks: "<<PP(area_offset_mapblocks)
			<<", area_size_mapblocks: "<<PP(area_size_mapblocks)
			<<", block_div: "<<PP(block_div)
			<<", node_ids.size(): "<<node_ids.size()
			<<", lights.size(): "<<lights.size()
			<<std::endl;

	// Convert to divisions (which will match FarMapNodes)
	v3s16 div_p0(
		area_offset_mapblocks.X * block_div.X,
		area_offset_mapblocks.Y * block_div.Y,
		area_offset_mapblocks.Z * block_div.Z
	);
	v3s16 div_p1 = div_p0 + v3s16(
		area_size_mapblocks.X * block_div.X,
		area_size_mapblocks.Y * block_div.Y,
		area_size_mapblocks.Z * block_div.Z
	);
	// This can be used for indexing node_ids and lights
	VoxelArea div_area(div_p0, div_p1 - v3s16(1,1,1));

	// Convert to FarMapBlock positions (this can cover extra area)
	VoxelArea fmb_area(
		getContainerPos(area_offset_mapblocks, FMP_SCALE),
		getContainerPos(area_offset_mapblocks + area_size_mapblocks -
				v3s16(1,1,1), FMP_SCALE)
	);

	v3s16 fbp;
	for (fbp.Y=fmb_area.MinEdge.Y; fbp.Y<=fmb_area.MaxEdge.Y; fbp.Y++)
	for (fbp.X=fmb_area.MinEdge.X; fbp.X<=fmb_area.MaxEdge.X; fbp.X++)
	for (fbp.Z=fmb_area.MinEdge.Z; fbp.Z<=fmb_area.MaxEdge.Z; fbp.Z++) {
		infostream<<"FarMap::insertData: FarBlock "<<PP(fbp)<<std::endl;

		FarMapBlock *b = getOrCreateBlock(fbp);
		
		b->resize(block_div);

		// Copy stuff into b
		v3s16 dp00(
			fbp.X * FMP_SCALE * block_div.X,
			fbp.Y * FMP_SCALE * block_div.Y,
			fbp.Z * FMP_SCALE * block_div.Z
		);
		v3s16 dp1;
		for (dp1.Y=0; dp1.Y<b->total_size.Y; dp1.Y++)
		for (dp1.X=0; dp1.X<b->total_size.X; dp1.X++)
		for (dp1.Z=0; dp1.Z<b->total_size.Z; dp1.Z++) {
			v3s16 dp0 = dp00 + dp1;
			// The source area does not necessarily contain all positions that
			// the matching blocks contain
			if(!div_area.contains(dp0))
				continue;
			size_t source_i = div_area.index(dp0);
			size_t dst_i = b->index(dp1);
			b->content[dst_i].id = node_ids[source_i];
			/*b->content[dst_i].id = ((dp0.X + dp0.Y + dp0.Z) % 3 == 0) ?
					5 : CONTENT_AIR;*/
			b->content[dst_i].light = lights[source_i];
		}

		/*// TODO: Remove this
		b->content[0].id = 5;
		b->content[10].id = 6;
		b->content[20].id = 7;
		b->content[30].id = 8;*/

		startGeneratingBlockMesh(b);
	}
}

void FarMap::startGeneratingBlockMesh(FarMapBlock *b)
{
	FarMapBlockMeshGenerateTask *t = new FarMapBlockMeshGenerateTask(this, *b);

	m_worker_thread.addTask(t);
}

void FarMap::insertGeneratedBlockMesh(v3s16 p, scene::SMesh *mesh)
{
	FarMapBlock *b = getOrCreateBlock(p);
	if(b->mesh)
		b->mesh->drop();
	mesh->grab();
	b->mesh = mesh;
	b->resetCameraOffset(m_camera_offset);

	g_profiler->add("Far: generated farblocks meshes", 1);
}

void FarMap::update()
{
	updateSettings();

	if (farblock_shader_id == 0 && config_enable_shaders) {
		// Fetch a basic node shader
		// NOTE: ShaderSource does not implement asynchronous fetching of
		// shaders from the main thread like TextureSource. While it probably
		// should do that, we can just fetch this id here for now as we use a
		// static shader anyway.
		u8 material_type = TILE_MATERIAL_BASIC;
		enum NodeDrawType drawtype = NDT_NORMAL;
		infostream<<"FarMapBlockMeshGenerate: Getting shader..."<<std::endl;
		IShaderSource *ssrc = client->getShaderSource();
		farblock_shader_id = ssrc->getShader(
				"nodes_shader", material_type, drawtype);
		infostream<<"FarMapBlockMeshGenerate: shader_id="<<farblock_shader_id
				<<std::endl;
	}

	m_worker_thread.sync();
}

void FarMap::updateCameraOffset(v3s16 camera_offset)
{
	if (camera_offset == m_camera_offset) return;

	m_camera_offset = camera_offset;

	for (std::map<v2s16, FarMapSector*>::iterator i = m_sectors.begin();
			i != m_sectors.end(); i++) {
		FarMapSector *s = i->second;

		for (std::map<s16, FarMapBlock*>::iterator i = s->blocks.begin();
				i != s->blocks.end(); i++) {
			FarMapBlock *b = i->second;
			b->updateCameraOffset(camera_offset);
		}
	}
}

void FarMap::updateSettings()
{
	config_enable_shaders = g_settings->getBool("enable_shaders");
	config_trilinear_filter = g_settings->getBool("trilinear_filter");
	config_bilinear_filter = g_settings->getBool("bilinear_filter");
	config_anistropic_filter = g_settings->getBool("anisotropic_filter");
}

static void renderBlock(FarMap *far_map, FarMapBlock *b, video::IVideoDriver* driver)
{
	scene::SMesh *mesh = b->mesh;
	if(!mesh){
		//infostream<<"FarMap::renderBlock: "<<PP(b->p)<<": No mesh"<<std::endl;
		return;
	}

	u32 c = mesh->getMeshBufferCount();
	/*infostream<<"FarMap::renderBlock: "<<PP(b->p)<<": Rendering "
			<<c<<" meshbuffers"<<std::endl;*/
	for(u32 i=0; i<c; i++)
	{
		scene::IMeshBuffer *buf = mesh->getMeshBuffer(i);
		buf->getMaterial().setFlag(video::EMF_TRILINEAR_FILTER,
				far_map->config_trilinear_filter);
		buf->getMaterial().setFlag(video::EMF_BILINEAR_FILTER,
				far_map->config_bilinear_filter);
		buf->getMaterial().setFlag(video::EMF_ANISOTROPIC_FILTER,
				far_map->config_anistropic_filter);

		const video::SMaterial& material = buf->getMaterial();

		driver->setMaterial(material);

		driver->drawMeshBuffer(buf);
	}
}

void FarMap::render()
{
	video::IVideoDriver* driver = SceneManager->getVideoDriver();
	driver->setTransform(video::ETS_WORLD, AbsoluteTransformation);

	size_t profiler_num_rendered_farblocks = 0;

	for (std::map<v2s16, FarMapSector*>::iterator i = m_sectors.begin();
			i != m_sectors.end(); i++) {
		FarMapSector *s = i->second;

		for (std::map<s16, FarMapBlock*>::iterator i = s->blocks.begin();
				i != s->blocks.end(); i++) {
			FarMapBlock *b = i->second;
			renderBlock(this, b, driver);

			profiler_num_rendered_farblocks++;
		}
	}

	g_profiler->avg("Far: rendered farblocks per frame",
			profiler_num_rendered_farblocks);
}

void FarMap::OnRegisterSceneNode()
{
	if(IsVisible)
	{
		SceneManager->registerNodeForRendering(this, scene::ESNRP_SOLID);
		//SceneManager->registerNodeForRendering(this, scene::ESNRP_TRANSPARENT);
	}

	ISceneNode::OnRegisterSceneNode();
}

const core::aabbox3d<f32>& FarMap::getBoundingBox() const
{
	return m_bounding_box;
}
