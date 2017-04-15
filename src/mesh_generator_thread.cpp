/*
Minetest
Copyright (C) 2013, 2017 celeron55, Perttu Ahola <celeron55@gmail.com>

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

#include "mesh_generator_thread.h"
#include "settings.h"
#include "profiler.h"
#include "client.h"
#include "mapblock.h"

#include "porting.h"
#include "profiler.h"
#define PROF_START \
		{ \
			u32 t0 = porting::getTime(PRECISION_MICRO);
#define PROF_ADD(desc) \
			u32 t1 = porting::getTime(PRECISION_MICRO); \
			g_profiler->graphAdd(desc " (s)", (t1 - t0) / 1000000.0); \
		}

/*
	CachedMapBlockData
*/

CachedMapBlockData::CachedMapBlockData():
	p(-1337,-1337,-1337),
	data(NULL),
	refcount_from_queue(1),
	last_used_timestamp(time(0))
{
}

CachedMapBlockData::~CachedMapBlockData()
{
	assert(refcount_from_queue == 0);

	if (data)
		delete[] data;
}

/*
	QueuedMeshUpdate
*/

QueuedMeshUpdate::QueuedMeshUpdate():
	p(-1337,-1337,-1337),
	ack_block_to_server(false),
	urgent(false),
	data(NULL)
{
}

QueuedMeshUpdate::~QueuedMeshUpdate()
{
	if (data)
		delete data;
}

/*
	MeshUpdateQueue
*/

MeshUpdateQueue::MeshUpdateQueue(Client *client):
	m_client(client)
{
	m_cache_enable_shaders = g_settings->getBool("enable_shaders");
	m_cache_use_tangent_vertices = m_cache_enable_shaders && (
		g_settings->getBool("enable_bumpmapping") ||
		g_settings->getBool("enable_parallax_occlusion"));
	m_cache_smooth_lighting = g_settings->getBool("smooth_lighting");
}

MeshUpdateQueue::~MeshUpdateQueue()
{
	MutexAutoLock lock(m_mutex);

	for(std::vector<QueuedMeshUpdate*>::iterator
			i = m_queue.begin();
			i != m_queue.end(); ++i)
	{
		QueuedMeshUpdate *q = *i;
		delete q;
	}
}

void MeshUpdateQueue::addBlock(MapBlock *b, bool ack_block_to_server, bool urgent)
{
	DSTACK(FUNCTION_NAME);

	v3s16 p = b->getPos();

	MutexAutoLock lock(m_mutex);

	cleanupCache();

	/*
		Cache the block data (update the cache if already cached)
	*/

	CachedMapBlockData *cached_block = NULL;
	UNORDERED_MAP<v3s16, CachedMapBlockData*>::iterator it =
			m_cache.find(p);
	if (it != m_cache.end()) {
		// Already in cache
		cached_block = it->second;
		memcpy(cached_block->data, b->getData(),
				MAP_BLOCKSIZE * MAP_BLOCKSIZE * MAP_BLOCKSIZE * sizeof(MapNode));
	} else {
		// Not yet in cache
		cached_block = new CachedMapBlockData();
		m_cache[p] = cached_block;
		cached_block->data = new MapNode[MAP_BLOCKSIZE * MAP_BLOCKSIZE * MAP_BLOCKSIZE];
		memcpy(cached_block->data, b->getData(),
				MAP_BLOCKSIZE * MAP_BLOCKSIZE * MAP_BLOCKSIZE * sizeof(MapNode));
		// The rest of this function assumes start from 0 for newly made caches
		cached_block->refcount_from_queue = 0;
	}

	// TODO: Fill missing neighbor blocks into cache (in the case they have been
	//       dropped after they were last updated)

	/*
		Mark the block as urgent if requested
	*/
	if(urgent)
		m_urgents.insert(p);

	/*
		Find if block is already in queue.
		If it is, update the data and quit.
	*/
	for(std::vector<QueuedMeshUpdate*>::iterator
			i = m_queue.begin();
			i != m_queue.end(); ++i)
	{
		QueuedMeshUpdate *q = *i;
		if(q->p == p)
		{
			// NOTE: We are not adding a new position to the queue, thus
			//       refcount_from_queue stays the same.
			if(ack_block_to_server)
				q->ack_block_to_server = true;
			return;
		}
	}

	/*
		Add the block
	*/
	QueuedMeshUpdate *q = new QueuedMeshUpdate;
	q->p = p;
	q->ack_block_to_server = ack_block_to_server;
	m_queue.push_back(q);

	// This queue entry is a new reference to the cached blocks
	cached_block->refcount_from_queue++;

	// TODO: Refer to neighbors
}

// Returned pointer must be deleted
// Returns NULL if queue is empty
QueuedMeshUpdate *MeshUpdateQueue::pop()
{
	MutexAutoLock lock(m_mutex);

	bool must_be_urgent = !m_urgents.empty();
	for(std::vector<QueuedMeshUpdate*>::iterator
			i = m_queue.begin();
			i != m_queue.end(); ++i)
	{
		QueuedMeshUpdate *q = *i;
		if(must_be_urgent && m_urgents.count(q->p) == 0)
			continue;
		m_queue.erase(i);
		m_urgents.erase(q->p);
		fillDataFromMapBlockCache(q);
		return q;
	}
	return NULL;
}

CachedMapBlockData* MeshUpdateQueue::getCachedBlock(const v3s16 &p)
{
	UNORDERED_MAP<v3s16, CachedMapBlockData*>::iterator it = m_cache.find(p);
	if (it != m_cache.end()) {
		return it->second;
	}
	return NULL;
}

void MeshUpdateQueue::fillDataFromMapBlockCache(QueuedMeshUpdate *q)
{
	PROF_START

	MeshMakeData *data = new MeshMakeData(m_client, m_cache_enable_shaders,
			m_cache_use_tangent_vertices);
	q->data = data;

	data->setSmoothLighting(m_cache_smooth_lighting);

	// TODO: Get these from somewhere (we're not in the main thread)
	//data->setCrack(m_client->getCrackLevel(), m_client->getCrackPos());

	data->fillBlockDataBegin(q->p);

	// Collect data for 3*3*3 blocks from cache
	v3s16 dp;
	for (dp.X = -1; dp.X <= 1; dp.X++) {
		for (dp.Y = -1; dp.Y <= 1; dp.Y++) {
			for (dp.Z = -1; dp.Z <= 1; dp.Z++) {
				v3s16 p = q->p + dp;
				CachedMapBlockData *cached_block = getCachedBlock(p);
				if (cached_block && cached_block->data)
					data->fillBlockData(dp, cached_block->data);
			}
		}
	}

	PROF_ADD("MeshUpdateQueue::fillDataFromMapBlockCache")
}

void MeshUpdateQueue::cleanupCache()
{
	g_profiler->avg("MeshUpdateQueue MapBlock cache size", m_cache.size());

	// TODO: Drop stuff that isn't referenced by the queue and hasn't been used
	//       for a while
}

/*
	MeshUpdateThread
*/

MeshUpdateThread::MeshUpdateThread(Client *client):
	UpdateThread("Mesh"),
	m_queue_in(client)
{
	m_generation_interval = g_settings->getU16("mesh_generation_interval");
	m_generation_interval = rangelim(m_generation_interval, 0, 50);
}

void MeshUpdateThread::updateBlock(MapBlock *b, bool ack_block_to_server, bool urgent)
{
	// Don't make MeshMakeData here but instead make a copy of the updated
	// MapBlock data and pass it to the mesh generator thread along with the
	// parameters

	m_queue_in.addBlock(b, ack_block_to_server, urgent);
	deferUpdate();
}

void MeshUpdateThread::doUpdate()
{
	QueuedMeshUpdate *q;
	while ((q = m_queue_in.pop())) {
		if (m_generation_interval)
			sleep_ms(m_generation_interval);
		ScopeProfiler sp(g_profiler, "Client: Mesh making");

		MapBlockMesh *mesh_new = new MapBlockMesh(q->data, m_camera_offset);

		MeshUpdateResult r;
		r.p = q->p;
		r.mesh = mesh_new;
		r.ack_block_to_server = q->ack_block_to_server;

		m_queue_out.push_back(r);

		delete q;
	}
}
