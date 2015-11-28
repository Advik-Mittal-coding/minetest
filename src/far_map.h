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
#ifndef __FAR_MAP_H__
#define __FAR_MAP_H__

#include "irrlichttypes_bloated.h"
#include "util/thread.h" // UpdateThread
#include <ISceneNode.h>
#include <SMesh.h>
#include <vector>
#include <map>

class Client;
//class ITextureSource;
struct FarMap;

// FarMapBlock size in MapBlocks in every dimension
#define FMP_SCALE 8

struct FarMapNode
{
	u16 id;
	u8 light;
};

struct FarMapBlock
{
	v3s16 p;

	// In how many pieces MapBlocks have been divided per dimension
	v3s16 block_div;
	// Total node dimensions of content
	v3s16 total_size;

	std::vector<FarMapNode> content;

	scene::SMesh *mesh;
	v3s16 current_camera_offset;

	FarMapBlock(v3s16 p);
	~FarMapBlock();

	void resize(v3s16 new_block_div);
	void updateCameraOffset(v3s16 camera_offset);
	void resetCameraOffset(v3s16 camera_offset = v3s16(0, 0, 0));

	size_t index(v3s16 p) {
		assert(p.X >= 0 && p.Y >= 0 && p.Z >= 0);
		assert(p.X < total_size.X && p.Y < total_size.Y && p.Z < total_size.Z);
		return p.Z * total_size.X * total_size.Y + p.Y * total_size.X + p.X;
	}
};

struct FarMapSector
{
	v2s16 p;

	std::map<s16, FarMapBlock*> blocks;

	FarMapSector(v2s16 p);
	~FarMapSector();

	FarMapBlock* getOrCreateBlock(s16 p);
};

struct FarMapTask
{
	virtual ~FarMapTask(){}
	virtual void inThread() = 0;
	virtual void sync() = 0;
};

struct FarMapBlockMeshGenerateTask: public FarMapTask
{
	FarMap *far_map;
	FarMapBlock source_block;
	scene::SMesh *mesh;

	FarMapBlockMeshGenerateTask(FarMap *far_map, const FarMapBlock &source_block);
	~FarMapBlockMeshGenerateTask();
	void inThread();
	void sync();
};

class FarMapWorkerThread: public UpdateThread
{
public:
	FarMapWorkerThread(): UpdateThread("FarMapWorker") {}
	~FarMapWorkerThread();

	void addTask(FarMapTask *task);
	void sync();

private:
	void doUpdate();

	MutexedQueue<FarMapTask*> m_queue_in;
	MutexedQueue<FarMapTask*> m_queue_sync;
	//v3s16 m_camera_offset; // TODO
};

class FarMap: public scene::ISceneNode
{
public:
	FarMap(
			Client *client,
			scene::ISceneNode* parent,
			scene::ISceneManager* mgr,
			s32 id
	);
	~FarMap();

	FarMapSector* getOrCreateSector(v2s16 p);
	FarMapBlock* getOrCreateBlock(v3s16 p);

	// Parameter dimensions are in MapBlocks
	void insertData(v3s16 area_offset_mapblocks, v3s16 area_size_mapblocks,
			v3s16 block_div,
			const std::vector<u16> &node_ids, const std::vector<u8> &lights);

	void startGeneratingBlockMesh(FarMapBlock *b);
	void insertGeneratedBlockMesh(v3s16 p, scene::SMesh *mesh);

	void update();
	void updateCameraOffset(v3s16 camera_offset);

	// ISceneNode methods
	void OnRegisterSceneNode();
	void render();
	const core::aabbox3d<f32>& getBoundingBox() const;

	Client *client;

	bool config_enable_shaders;
	bool config_trilinear_filter;
	bool config_bilinear_filter;
	bool config_anistropic_filter;

	u32 farblock_shader_id;

private:
	void updateSettings();

	FarMapWorkerThread m_worker_thread;

	// Source data
	std::map<v2s16, FarMapSector*> m_sectors;

	// Rendering stuff
	core::aabbox3d<f32> m_bounding_box;
	v3s16 m_camera_offset;
};

#endif
