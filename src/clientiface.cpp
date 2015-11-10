/*
Minetest
Copyright (C) 2010-2014 celeron55, Perttu Ahola <celeron55@gmail.com>

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

#include <sstream>

#include "clientiface.h"
#include "util/numeric.h"
#include "util/mathconstants.h"
#include "player.h"
#include "settings.h"
#include "mapblock.h"
#include "network/connection.h"
#include "environment.h"
#include "map.h"
#include "emerge.h"
#include "serverobject.h"              // TODO this is used for cleanup of only
#include "log.h"
#include "util/srp.h"

/*
	AutosendAlgorithm
*/

u16 figure_out_max_simultaneous_block_sends(u16 base_setting,
		float time_from_building, float time_from_building_limit_setting,
		s16 block_distance_in_blocks)
{
	// If block is very close, always send the configured amount
	if (block_distance_in_blocks <= BLOCK_SEND_DISABLE_LIMITS_MAX_D)
		return base_setting;

	// If the time from last addNode/removeNode is small, don't send as much
	// stuff in order to reduce lag
	if (time_from_building < time_from_building_limit_setting)
		return LIMITED_MAX_SIMULTANEOUS_BLOCK_SENDS;

	// Send the configured amount if nothing special is happening
	return base_setting;
}

void AutosendAlgorithm::Cycle::init(AutosendAlgorithm *alg_,
		RemoteClient *client_, ServerEnvironment *env_)
{
	//dstream<<"AutosendAlgorithm::Cycle::init()"<<std::endl;

	alg = alg_;
	client = client_;
	env = env_;

	disabled = false;
	i = 0;

	if (alg->m_radius_map == 0 && alg->m_radius_far == 0) {
		//dstream<<"radius=0"<<std::endl;
		disabled = true;
		return;
	}

	// TODO: Enable
	/*if (alg->m_nothing_to_send_pause_timer >= 0) {
		dstream<<"nothing to send pause"<<std::endl;
		disabled = true;
		return;
	}*/

	Player *player = env->getPlayer(client->peer_id);
	if (player == NULL) {
		//dstream<<"player==NULL"<<std::endl;
		// This can happen sometimes; clients and players are not in perfect sync.
		disabled = true;
		return;
	}

	// Won't send anything if already sending
	if (client->m_blocks_sending.size() >= g_settings->getU16
			("max_simultaneous_block_sends_per_client")) {
		//dstream<<"max_simul_sends"<<std::endl;
		//infostream<<"Not sending any blocks, Queue full."<<std::endl;
		disabled = true;
		return;
	}

	camera_p = player->getEyePosition();
	v3f player_speed = player->getSpeed();

	// Get player position and figure out a good focus point for block selection
	v3f player_speed_dir(0,0,0);
	if(player_speed.getLength() > 1.0*BS)
		player_speed_dir = player_speed / player_speed.getLength();
	// Predict to next block
	v3f camera_p_predicted = camera_p + player_speed_dir*MAP_BLOCKSIZE*BS;
	v3s16 focus_point_nodepos = floatToInt(camera_p_predicted, BS);
	focus_point = getNodeBlockPos(focus_point_nodepos);

	// Camera position and direction
	camera_dir = v3f(0,0,1);
	camera_dir.rotateYZBy(player->getPitch());
	camera_dir.rotateXZBy(player->getYaw());

	// If focus point has changed to a different MapBlock, reset radius value
	// for iterating from zero again
	if (alg->m_last_focus_point != focus_point) {
		alg->m_nearest_unsent_d = 0;
		alg->m_last_focus_point = focus_point;
	}

	// Get some settings
	max_simul_sends_setting =
			g_settings->getU16("max_simultaneous_block_sends_per_client");
	time_from_building_limit_s =
			g_settings->getFloat("full_block_send_enable_min_time_from_building");
	max_block_send_distance_setting =
			g_settings->getS16("max_block_send_distance");
	max_block_generate_distance =
			g_settings->getS16("max_block_generate_distance");

	// Derived settings
	max_block_send_distance =
			alg->m_radius_map < max_block_send_distance_setting ?
			alg->m_radius_map : max_block_send_distance_setting;

	// Reset periodically to workaround possible glitches due to whatever
	// reasons (this is somewhat guided by just heuristics, after all)
	if (alg->m_nearest_unsent_reset_timer > 20.0) {
		alg->m_nearest_unsent_reset_timer = 0;
		alg->m_nearest_unsent_d = 0;
	}

	// Out-of-FOV distance limit
	// TODO: FarBlocks should use this multiplied by alg->m_far_weight
	fov_limit_activation_distance = alg->m_fov_limit_enabled ?
			max_block_send_distance / 2 : max_block_send_distance;

	/*
		MapBlock iteration
	*/

	// We will start from a radius that still has unsent MapBlocks
	mapblock.d_start = alg->m_mapblock.nearest_unsent_d >= 0 ?
			alg->m_mapblock.nearest_unsent_d : 0;
	mapblock.d = mapblock.d_start;

	//dstream<<"mapblock.d_start="<<mapblock.d_start<<std::endl;
	//infostream<<"mapblock.d_start="<<mapblock.d_start<<std::endl;

	// Don't loop very much at a time. This function is called each server tick
	// so just a few steps will work fine (+2 is 3 steps per call).
	mapblock.d_max = 0;
	if (mapblock.d_start < 5)
		mapblock.d_max = mapblock.d_start + 2;
	else if (mapblock.d_max < 8)
		mapblock.d_max = mapblock.d_start + 1;
	else
		mapblock.d_max = mapblock.d_start; // These iterations start to be rather heavy

	if (mapblock.d_max > max_block_send_distance)
		mapblock.d_max = max_block_send_distance;

	//dstream<<"mapblock.d_max="<<mapblock.d_max<<std::endl;

	// Keep track of... things. We need these in order to figure out where to
	// continue iterating on the next call to this function.
	// TODO: Put these in the class
	mapblock.nearest_emergequeued_d = INT32_MAX;
	mapblock.nearest_emergefull_d = INT32_MAX;
	mapblock.nearest_sendqueued_d = INT32_MAX;

	/*
		FarBlock iteration
	*/

	// We will start from a radius that still has unsent FarBlocks
	farblock.d_start = alg->m_farblock.nearest_unsent_d >= 0 ?
			alg->m_farblock.nearest_unsent_d : 0;
	farblock.d = farblock.d_start;

	//dstream<<"farblock.d_start="<<farblock.d_start<<std::endl;
	//infostream<<"farblock.d_start="<<farblock.d_start<<std::endl;

	// Don't loop very much at a time. This function is called each server tick
	// so just a few steps will work fine (+2 is 3 steps per call).
	farblock.d_max = 0;
	if (farblock.d_start < 5)
		farblock.d_max = farblock.d_start + 2;
	else if (farblock.d_max < 8)
		farblock.d_max = farblock.d_start + 1;
	else
		farblock.d_max = farblock.d_start; // These iterations start to be rather heavy

	if (farblock.d_max > max_block_send_distance)
		farblock.d_max = max_block_send_distance;

	//dstream<<"farblock.d_max="<<farblock.d_max<<std::endl;

	// Keep track of... things. We need these in order to figure out where to
	// continue iterating on the next call to this function.
	// TODO: Put these in the class
	farblock.nearest_emergequeued_d = INT32_MAX;
	farblock.nearest_emergefull_d = INT32_MAX;
	farblock.nearest_sendqueued_d = INT32_MAX;
}

WantedMapSend AutosendAlgorithm::Cycle::suggestNextMapBlock(
		bool *result_needs_emerge)
{
	for (; mapblock.d <= mapblock.d_max; mapblock.d++) {
		//dstream<<"mapblock.d="<<mapblock.d<<std::endl;
		// Get the border/face dot coordinates of a mapblock.d-"radiused" box
		std::vector<v3s16> face_ps = FacePositionCache::getFacePositions(mapblock.d);
		// Continue from the last mapblock.i unless it was reset by something
		for (; mapblock.i < face_ps.size(); mapblock.i++) {
			v3s16 p = face_ps[mapblock.i] + focus_point;
			WantedMapSend wms(WMST_MAPBLOCK, p);

			// Limit fetched MapBlocks to a ball radius instead of a square
			// because that is how they are limited when drawing too
			v3s16 blockpos_nodes = p * MAP_BLOCKSIZE;
			v3f blockpos_center(
					blockpos_nodes.X * BS + MAP_BLOCKSIZE/2 * BS,
					blockpos_nodes.Y * BS + MAP_BLOCKSIZE/2 * BS,
					blockpos_nodes.Z * BS + MAP_BLOCKSIZE/2 * BS
			);
			v3f blockpos_relative = blockpos_center - camera_p;
			f32 distance = blockpos_relative.getLength();
			if (distance > max_block_send_distance * MAP_BLOCKSIZE * BS) {
				//dstream<<"continue: distance"<<std::endl;
				continue; // Not in range
			}

			// Calculate this thing
			u16 max_simultaneous_block_sends =
					figure_out_max_simultaneous_block_sends(
							max_simul_sends_setting,
							client->m_time_from_building,
							time_from_building_limit_s,
							mapblock.d);

			// Don't select too many blocks for sending
			if (num_blocks_selected >= max_simultaneous_block_sends) {
				//dstream<<"return: num_selected"<<std::endl;
				return WantedMapSend();
			}

			// Don't send blocks that are currently being transferred
			if (client->m_blocks_sending.count(wms)) {
				//dstream<<"continue: num sending"<<std::endl;
				continue;
			}

			// Don't go over hard map limits
			if (blockpos_over_limit(p)) {
				//dstream<<"continue: over limit"<<std::endl;
				continue;
			}

			// If this is true, inexistent blocks will be made from scratch
			bool generate_allowed = mapblock.d <= max_block_generate_distance;

			/*// Limit the generating area vertically to 2/3
			if(abs(p.Y - focus_point.Y) >
					max_block_generate_distance - max_block_generate_distance / 3)
				generate_allowed = false;*/

			/*// Limit the send area vertically to 1/2
			if (abs(p.Y - focus_point.Y) > max_block_send_distance / 2)
				continue;*/

			if (mapblock.d >= fov_limit_activation_distance) {
				// Don't generate or send if not in sight
				if(isBlockInSight(p, camera_p, camera_dir, alg->m_fov,
						10000*BS) == false)
				{
					//dstream<<"continue: not in sight"<<std::endl;
					continue;
				}
			}

			// Don't send blocks that have already been sent
			if (client->m_blocks_sent.count(wms)) {
				//dstream<<"continue: already sent"<<std::endl;
				continue;
			}

			/*
				Check if map has this block
			*/
			MapBlock *block = env->getMap().getBlockNoCreateNoEx(p);

			bool surely_not_found_on_disk = false;
			bool emerge_required = false;
			if(block != NULL)
			{
				// Reset usage timer, this block will be of use in the future.
				block->resetUsageTimer();

				// Block is dummy if data doesn't exist.
				// It means it has been not found from disk and not generated
				if(block->isDummy())
					surely_not_found_on_disk = true;

				// Block is valid if lighting is up-to-date and data exists
				if(block->isValid() == false)
					emerge_required = true;

				// If a block hasn't been generated but we would ask it to be
				// generated, it's invalid.
				if(block->isGenerated() == false && generate_allowed)
					emerge_required = true;

				// This check is disabled because it mis-guesses sea floors to
				// not be worth transferring to the client, while they are.
				/*if (mapblock.d >= 4 && block->getDayNightDiff() == false)
					continue;*/
			}

			/*
				If block has been marked to not exist on disk (dummy)
				and generating new ones is not wanted, skip block.
			*/
			if(generate_allowed == false && surely_not_found_on_disk == true)
			{
				//dstream<<"continue: not on disk, no generate"<<std::endl;
				// get next one.
				continue;
			}

			/*
				Add inexistent block to emerge queue.
			*/
			if(block == NULL || surely_not_found_on_disk || emerge_required)
			{
				if (result_needs_emerge)
					*result_needs_emerge = true;
			}

			// Suggest this block
			return wms;
		}

		// Now reset i as we go to next d level
		mapblock.i = 0;
	}

	// Nothing to suggest
	return WantedMapSend();
}

WantedMapSend AutosendAlgorithm::Cycle::suggestNextFarBlock()
{
	v3s16 focus_point_fb = getContainerPos(focus_point, FMP_SCALE);

	// Avoid running the algorithm through all the close FarBlocks that probably
	// have already been fetched, except once in a while to catch up with
	// possible missed FarBlocks due to player movement or whatever.
	s16 start_d = m_farblocks_exist_up_to_d; // Start one lower than have to
	if (++m_farblocks_exist_up_to_d_reset_counter >= 10) {
		m_farblocks_exist_up_to_d_reset_counter = 0;
		start_d = 0;
	}
	m_farblocks_exist_up_to_d = -1; // Reset and recalculate
	if (start_d < 0)
		start_d = 0;

	for (s16 d = start_d; d <= alg->m_radius_far; d++) {
		std::vector<v3s16> ps = FacePositionCache::getFacePositions(d);
		for (size_t i=0; i<ps.size(); i++) {
			v3s16 p = focus_point_fb + ps[i];
			FarBlock *b = getBlock(p);
			/*dstream<<"FarMap::suggestFarBlocksToFetch: "
					<<PP(p)<<" = "<<b<<std::endl;*/
			if (b != NULL) {
				if (!b->load_in_progress_on_server) {
					//dstream<<"* "<<PP(p)<<" is fully loaded"<<std::endl;
					continue; // Exists and was received fully loaded
				}
				b->refresh_from_server_counter++;
				if (b->refresh_from_server_counter < 5) {
					/*dstream<<"* "<<PP(p)<<" is being loaded on server; counting "
							<<b->refresh_from_server_counter<<std::endl;*/
					continue; // Don't try reloading this time
				}
				dstream<<"* "<<PP(p)<<" is being loaded on server"
						<<"; asking update from server"<<std::endl;
				// Try reloading this time
				b->refresh_from_server_counter = 0;
			} else {
				dstream<<"* "<<PP(p)<<" is unfetched"
						<<"; asking from server"<<std::endl;
			}
			if (m_farblocks_exist_up_to_d == -1)
				m_farblocks_exist_up_to_d = d - 1;
			suggested_fbs.push_back(p);
			if (suggested_fbs.size() >= wanted_num_results)
				goto done;
		}
	}

	for (; farblock.d <= farblock.d_max; farblock.d++) {
		//dstream<<"farblock.d="<<farblock.d<<std::endl;
		// Get the border/face dot coordinates of a farblock.d-"radiused" box
		std::vector<v3s16> face_ps = FacePositionCache::getFacePositions(farblock.d);
		// Continue from the last farblock.i unless it was reset by something
		for (; farblock.i < face_ps.size(); farblock.i++) {
			v3s16 p = focus_point_fb + face_ps[farblock.i];

			WantedMapSend wms(WMST_MAPBLOCK, p);

			FarBlock *b = getBlock(p);

			// Calculate this thing
			u16 max_simultaneous_block_sends =
					figure_out_max_simultaneous_block_sends(
							max_simul_sends_setting,
							client->m_time_from_building,
							time_from_building_limit_s,
							farblock.d);

			// Don't select too many blocks for sending
			if (num_blocks_selected >= max_simultaneous_block_sends) {
				//dstream<<"return: num_selected"<<std::endl;
				return WantedMapSend();
			}

			// Don't send blocks that are currently being transferred
			if (client->m_blocks_sending.count(wms)) {
				//dstream<<"continue: num sending"<<std::endl;
				continue;
			}

			// Don't go over hard map limits
			if (blockpos_over_limit(p)) {
				//dstream<<"continue: over limit"<<std::endl;
				continue;
			}

			// If this is true, inexistent blocks will be made from scratch
			bool generate_allowed = farblock.d <= max_block_generate_distance;

			/*// Limit the generating area vertically to 2/3
			if(abs(p.Y - focus_point.Y) >
					max_block_generate_distance - max_block_generate_distance / 3)
				generate_allowed = false;*/

			/*// Limit the send area vertically to 1/2
			if (abs(p.Y - focus_point.Y) > max_block_send_distance / 2)
				continue;*/

			if (farblock.d >= fov_limit_activation_distance) {
				// Don't generate or send if not in sight
				if(isBlockInSight(p, camera_p, camera_dir, alg->m_fov,
						10000*BS) == false)
				{
					//dstream<<"continue: not in sight"<<std::endl;
					continue;
				}
			}

			// Don't send blocks that have already been sent
			if (client->m_blocks_sent.count(wms)) {
				//dstream<<"continue: already sent"<<std::endl;
				continue;
			}

			/*
				Check if map has this block
			*/
			MapBlock *block = env->getMap().getBlockNoCreateNoEx(p);

			bool surely_not_found_on_disk = false;
			bool emerge_required = false;
			if(block != NULL)
			{
				// Reset usage timer, this block will be of use in the future.
				block->resetUsageTimer();

				// Block is dummy if data doesn't exist.
				// It means it has been not found from disk and not generated
				if(block->isDummy())
					surely_not_found_on_disk = true;

				// Block is valid if lighting is up-to-date and data exists
				if(block->isValid() == false)
					emerge_required = true;

				// If a block hasn't been generated but we would ask it to be
				// generated, it's invalid.
				if(block->isGenerated() == false && generate_allowed)
					emerge_required = true;

				// This check is disabled because it mis-guesses sea floors to
				// not be worth transferring to the client, while they are.
				/*if (farblock.d >= 4 && block->getDayNightDiff() == false)
					continue;*/
			}

			/*
				If block has been marked to not exist on disk (dummy)
				and generating new ones is not wanted, skip block.
			*/
			if(generate_allowed == false && surely_not_found_on_disk == true)
			{
				//dstream<<"continue: not on disk, no generate"<<std::endl;
				// get next one.
				continue;
			}

			/*
				Add inexistent block to emerge queue.
			*/
			if(block == NULL || surely_not_found_on_disk || emerge_required)
			{
				if (result_needs_emerge)
					*result_needs_emerge = true;
			}

			// Suggest this block
			return wms;
		}

		// Now reset i as we go to next d level
		farblock.i = 0;
	}

	// Nothing to suggest
	return WantedMapSend();
}

WantedMapSend AutosendAlgorithm::Cycle::getNextBlock(EmergeManager *emerge)
{
	DSTACK(FUNCTION_NAME);

	dstream<<"AutosendAlgorithm::Cycle::getNextBlock()"<<std::endl;

	if (disabled)
		return WantedMapSend();

	u32 num_blocks_selected = client->m_blocks_sending.size();

	// TODO: Get FarBlocks

	for (; d <= d_max; d++) {
		//dstream<<"d="<<d<<std::endl;
		// Get the border/face dot coordinates of a d-"radiused" box
		std::vector<v3s16> face_ps = FacePositionCache::getFacePositions(d);
		// Continue from the last i unless it was reset by something
		for (; i < face_ps.size(); i++) {
			v3s16 p = face_ps[i] + focus_point;
			WantedMapSend wms(WMST_MAPBLOCK, p);

			// Limit fetched MapBlocks to a ball radius instead of a square
			// because that is how they are limited when drawing too
			v3s16 blockpos_nodes = p * MAP_BLOCKSIZE;
			v3f blockpos_center(
					blockpos_nodes.X * BS + MAP_BLOCKSIZE/2 * BS,
					blockpos_nodes.Y * BS + MAP_BLOCKSIZE/2 * BS,
					blockpos_nodes.Z * BS + MAP_BLOCKSIZE/2 * BS
			);
			v3f blockpos_relative = blockpos_center - camera_p;
			f32 distance = blockpos_relative.getLength();
			if (distance > max_block_send_distance * MAP_BLOCKSIZE * BS) {
				//dstream<<"continue: distance"<<std::endl;
				continue; // Not in range
			}

			// Calculate this thing
			u16 max_simultaneous_block_sends =
					figure_out_max_simultaneous_block_sends(
							max_simul_sends_setting,
							client->m_time_from_building,
							time_from_building_limit_s,
							d);

			// Don't select too many blocks for sending
			if (num_blocks_selected >= max_simultaneous_block_sends) {
				//dstream<<"return: num_selected"<<std::endl;
				return WantedMapSend();
			}

			// Don't send blocks that are currently being transferred
			if (client->m_blocks_sending.count(wms)) {
				//dstream<<"continue: num sending"<<std::endl;
				continue;
			}

			// Don't go over hard map limits
			if (blockpos_over_limit(p)) {
				//dstream<<"continue: over limit"<<std::endl;
				continue;
			}

			// If this is true, inexistent blocks will be made from scratch
			bool generate_allowed = d <= max_block_generate_distance;

			/*// Limit the generating area vertically to 2/3
			if(abs(p.Y - focus_point.Y) >
					max_block_generate_distance - max_block_generate_distance / 3)
				generate_allowed = false;*/

			/*// Limit the send area vertically to 1/2
			if (abs(p.Y - focus_point.Y) > max_block_send_distance / 2)
				continue;*/

			if (d >= fov_limit_activation_distance) {
				// Don't generate or send if not in sight
				if(isBlockInSight(p, camera_p, camera_dir, alg->m_fov,
						10000*BS) == false)
				{
					//dstream<<"continue: not in sight"<<std::endl;
					continue;
				}
			}

			// Don't send blocks that have already been sent
			if (client->m_blocks_sent.count(wms)) {
				//dstream<<"continue: already sent"<<std::endl;
				continue;
			}

			/*
				Check if map has this block
			*/
			MapBlock *block = env->getMap().getBlockNoCreateNoEx(p);

			bool surely_not_found_on_disk = false;
			bool emerge_required = false;
			if(block != NULL)
			{
				// Reset usage timer, this block will be of use in the future.
				block->resetUsageTimer();

				// Block is dummy if data doesn't exist.
				// It means it has been not found from disk and not generated
				if(block->isDummy())
					surely_not_found_on_disk = true;

				// Block is valid if lighting is up-to-date and data exists
				if(block->isValid() == false)
					emerge_required = true;

				// If a block hasn't been generated but we would ask it to be
				// generated, it's invalid.
				if(block->isGenerated() == false && generate_allowed)
					emerge_required = true;

				// This check is disabled because it mis-guesses sea floors to
				// not be worth transferring to the client, while they are.
				/*if (d >= 4 && block->getDayNightDiff() == false)
					continue;*/
			}

			/*
				If block has been marked to not exist on disk (dummy)
				and generating new ones is not wanted, skip block.
			*/
			if(generate_allowed == false && surely_not_found_on_disk == true)
			{
				//dstream<<"continue: not on disk, no generate"<<std::endl;
				// get next one.
				continue;
			}

			/*
				Add inexistent block to emerge queue.
			*/
			if(block == NULL || surely_not_found_on_disk || emerge_required)
			{
				if (emerge->enqueueBlockEmerge(
						client->peer_id, p, generate_allowed)) {
					if (nearest_emergequeued_d == INT32_MAX)
						nearest_emergequeued_d = d;
				} else {
					if (nearest_emergefull_d == INT32_MAX)
						nearest_emergefull_d = d;
					return WantedMapSend();
				}

				// get next one.
				//dstream<<"continue: emerging"<<std::endl;
				continue;
			}

			if(nearest_sendqueued_d == INT32_MAX)
				nearest_sendqueued_d = d;

			// Select this block
			alg->m_nothing_sent_timer = 0.0f;
			return wms;
		}

		// Now reset i
		i = 0;
	}
	//dstream<<"return: nothing found"<<std::endl;
	return WantedMapSend();
}

SearchCycleResult AutosendAlgorithm::Cycle::finishSearchCycle(Search *search)
{
	/*infostream << "nearest_emergequeued_d = "<<search->nearest_emergequeued_d
			<<", nearest_emergefull_d = "<<search->nearest_emergefull_d
			<<", nearest_sendqueued_d = "<<search->nearest_sendqueued_d
			<<std::endl;*/

	// Because none of the things we queue for sending or emerging or anything
	// will necessarily be sent or anything, next time we need to continue
	// iterating from the closest radius of any of that happening so that we can
	// check whether they were sent or emerged or otherwise handled.
	s32 closest_required_re_check = INT32_MAX;
	if (search->nearest_emergequeued_d < closest_required_re_check)
		closest_required_re_check = search->nearest_emergequeued_d;
	if (search->nearest_emergefull_d < closest_required_re_check)
		closest_required_re_check = search->nearest_emergefull_d;
	if (search->nearest_sendqueued_d < closest_required_re_check)
		closest_required_re_check = search->nearest_sendqueued_d;

	SearchCycleResult r;

	if (closest_required_re_check != INT32_MAX) {
		// We did something that requires a result to be checked later. Continue
		// from that on the next time.
		r.nearest_unsent_d = closest_required_re_check;
		// If nothing has been sent in a moment, indicating that the emerge
		// thread is not finding anything on disk anymore, caller should start a
		// fresh pass without the FOV limit.
		r.result_may_be_available_later_at_this_d = true;
	} else if (d > max_block_send_distance) {
		// We iterated all the way through to the end of the send radius, if you
		// can believe that.
		r.nearest_unsent_d = 0;
		r.searched_full_range = true;
		// Caller should do a second pass with FOV limiting disabled or start
		// from the beginning after a short idle delay. (with FOV limiting
		// enabled because nobody knows what the future holds.)
		r.result_may_be_available_later_at_this_d = true;
	} else {
		// Absolutely nothing interesting happened. Next time we will continue
		// iterating from the next radius.
		r.nearest_unsent_d = d;
	}

	return r;
}

void AutosendAlgorithm::Cycle::finish()
{
	//dstream<<"AutosendAlgorithm::Cycle::finish()"<<std::endl;

	if (disabled) {
		// If disabled, none of our variables are even initialized
		return;
	}

	// Handle MapBlock search
	{
		SearchCycleResult r = finishSearchCycle(&mapblock);

		// Default to continuing iterating from the next radius next time.
		alg->m_mapblock.nearest_unsent_d = r.nearest_unsent_d;

		// Trigger FOV limit removal in certain situations
		if (r.result_may_be_available_later_at_this_d) {
			// If nothing has been sent in a moment, indicating that the emerge
			// thread is not finding anything on disk anymore, start a fresh
			// pass without the FOV limit.
			if (alg->m_fov_limit_enabled && alg->m_nothing_sent_timer >= 3.0f &&
					alg->m_fov != 0.0f && alg->m_fov_limit_enabled) {
				alg->m_fov_limit_enabled = false;
				// Have to be reset in order to not trigger this immediately again
				alg->m_nothing_sent_timer = 0.0f;
			}
		} else if(r.searched_full_range) {
			// We iterated all the way through to the end of the send radius, if you
			// can believe that.
			if (alg->m_fov != 0.0f && alg->m_fov_limit_enabled) {
				// Do a second pass with FOV limiting disabled
				alg->m_fov_limit_enabled = false;
			} else {
				// Start from the beginning after a short idle delay, with FOV
				// limiting enabled because nobody knows what the future holds.
				alg->m_fov_limit_enabled = true;
				alg->m_nothing_to_send_pause_timer = 2.0;
			}
		}
	}

	// Handle FarBlock search
	{
		SearchCycleResult r = finishSearchCycle(&farblock);

		// Default to continuing iterating from the next radius next time.
		alg->m_farblock.nearest_unsent_d = r.nearest_unsent_d;

		// We don't really care about the result otherwise
	}
}

WantedMapSend AutosendAlgorithm::getNextBlock(EmergeManager *emerge)
{
	return m_cycle.getNextBlock(emerge);
}

void AutosendAlgorithm::cycle(float dtime, ServerEnvironment *env)
{
	m_cycle.finish();

	// Increment timers
	m_nothing_sent_timer += dtime;
	m_nearest_unsent_reset_timer += dtime;
	m_nothing_to_send_pause_timer -= dtime;

	m_cycle.init(this, m_client, env);
}

std::string AutosendAlgorithm::describeStatus()
{
	return "(m_nearest_unsent_d="+itos(m_nearest_unsent_d)+")";
}

/*
	ClientInterface
*/

const char *ClientInterface::statenames[] = {
	"Invalid",
	"Disconnecting",
	"Denied",
	"Created",
	"AwaitingInit2",
	"HelloSent",
	"InitDone",
	"DefinitionsSent",
	"Active",
	"SudoMode",
};

std::string ClientInterface::state2Name(ClientState state)
{
	return statenames[state];
}

void RemoteClient::ResendBlockIfOnWire(const WantedMapSend &wms)
{
	// if this block is on wire, mark it for sending again as soon as possible
	if (m_blocks_sending.find(wms) != m_blocks_sending.end()) {
		SetBlockUpdated(wms);
	}
}

WantedMapSend RemoteClient::getNextBlock(EmergeManager *emerge)
{
	// If the client has not indicated it supports the new algorithm, fill in
	// autosend parameters and things should work fine
	if (m_fallback_autosend_active)
	{
		s16 radius_map = g_settings->getS16("max_block_send_distance");
		s16 radius_far = 0; // Old client does not understand FarBlocks
		float far_weight = 8.0f; // Whatever non-zero
		float fov = 72.0f; // Assume something
		m_autosend.setParameters(radius_map, radius_far, far_weight, fov);

		// Continue normally.
	}

	/*
		Autosend

		NOTE: All auto-sent stuff is considered higher priority than custom
		transfers. If the client wants to get custom stuff quickly, it has to
		disable autosend.
	*/
	WantedMapSend wms = m_autosend.getNextBlock(emerge);
	if (wms.type != WMST_INVALID)
		return wms;

	/*
		Handle map send queue as set by the client for custom map transfers
	*/
	for (size_t i=0; i<m_map_send_queue.size(); i++) {
		const WantedMapSend &wms = m_map_send_queue[i];
		if (wms.type == WMST_MAPBLOCK) {
			/*verbosestream << "Server: Client "<<peer_id<<" wants MapBlock ("
					<<wms.p.X<<","<<wms.p.Y<<","<<wms.p.Z<<")"
					<< std::endl;*/

			// Do not go over-limit
			if (blockpos_over_limit(wms.p))
				continue;

			// Don't send blocks that are currently being transferred
			if (m_blocks_sending.find(wms) != m_blocks_sending.end())
				continue;

			// Don't send blocks that have already been sent, unless it has been
			// updated
			if (m_blocks_sent.find(wms) != m_blocks_sent.end()) {
				if (m_blocks_updated_since_last_send.count(wms) == 0)
					continue;
			}

			// If the MapBlock is not loaded, it will be queued to be loaded or
			// generated. Otherwise it will be added to 'dest'.

			const bool generate_allowed = true;

			MapBlock *block = m_env->getMap().getBlockNoCreateNoEx(wms.p);

			bool surely_not_found_on_disk = false;
			bool emerge_required = false;
			if(block != NULL)
			{
				// Reset usage timer, this block will be of use in the future.
				block->resetUsageTimer();

				// Block is dummy if data doesn't exist.
				// It means it has been not found from disk and not generated
				if(block->isDummy())
					surely_not_found_on_disk = true;

				// Block is valid if lighting is up-to-date and data exists
				if(block->isValid() == false)
					emerge_required = true;

				if(block->isGenerated() == false && generate_allowed)
					emerge_required = true;

				// This check is disabled because it mis-guesses sea floors to
				// not be worth transferring to the client, while they are.
				/*if (d >= 4 && block->getDayNightDiff() == false)
					continue;*/
			}

			if(generate_allowed == false && surely_not_found_on_disk == true) {
				// NOTE: If we wanted to avoid generating new blocks based on
				// some criterion, that check woould go here and we would call
				// 'continue' if the block should not be generated.

				// TODO: There needs to be some new way or limiting which
				// positions are allowed to be generated, because we aren't
				// going to look up the player's position and compare it with
				// max_block_generate_distance anymore. Maybe a configured set
				// of allowed areas, or maybe a callback to Lua.

				// NOTE: There may need to be a way to tell the client that this
				// block will not be transferred to it no matter how nicely it
				// asks. Otherwise the requested send queue is going to get
				// filled up by these and less important blocks further away
				// that happen to have been already generated will not transfer.
			}

			/*
				If block does not exist, add it to the emerge queue.
			*/
			if(block == NULL || surely_not_found_on_disk || emerge_required)
			{
				bool allow_generate = true;
				if (!emerge->enqueueBlockEmerge(peer_id, wms.p, allow_generate)) {
					// EmergeThread's queue is full; maybe it's not full on the
					// next time this is called.
				}

				// This block is not available now; hopefully it appears on some
				// next call to this function.
				continue;
			}

			// The block is available
			return wms;
		}
		if (wms.type == WMST_FARBLOCK) {
			/*verbosestream << "Server: Client "<<peer_id<<" wants FarBlock ("
					<<wms.p.X<<","<<wms.p.Y<<","<<wms.p.Z<<")"
					<< std::endl;*/

			// Do not go over-limit
			if (blockpos_over_limit(wms.p))
				continue;

			// Don't send blocks that are currently being transferred
			if (m_blocks_sending.find(wms) != m_blocks_sending.end())
				continue;

			// Don't send blocks that have already been sent
			std::map<WantedMapSend, time_t>::const_iterator
					blocks_sent_i = m_blocks_sent.find(wms);
			if (blocks_sent_i != m_blocks_sent.end()){
				if (m_blocks_updated_since_last_send.count(wms) == 0) {
					dstream<<"RemoteClient: Already sent and not updated: "
							<<"("<<wms.p.X<<","<<wms.p.Y<<","<<wms.p.Z<<")"
							<<std::endl;
					continue;
				}
				time_t sent_time = blocks_sent_i->second;
				if (sent_time + 5 > time(NULL)) {
					/*dstream<<"RemoteClient: Already sent; rate-limiting: "
							<<"("<<wms.p.X<<","<<wms.p.Y<<","<<wms.p.Z<<")"
							<<std::endl;*/
					continue;
				}
				dstream<<"RemoteClient: Already sent but updated; allowing re-send: "
						<<"("<<wms.p.X<<","<<wms.p.Y<<","<<wms.p.Z<<")"
						<<std::endl;
			}

			// TODO: Stay determined at which FarBlock to load so that we don't
			//       load them all at the same time in weird stripes

			// TODO: Use modification counter?

			// TODO: Check if data for this is available and if not, possibly
			//       request an emerge of the required area

			return wms;
		}
	}

	return WantedMapSend();
}

void RemoteClient::cycleAutosendAlgorithm(float dtime)
{
	m_autosend.cycle(dtime, m_env);
}

void RemoteClient::GotBlock(const WantedMapSend &wms)
{
	if (m_blocks_sending.find(wms) != m_blocks_sending.end()) {
		m_blocks_sent[wms] = m_blocks_sending[wms];
		m_blocks_sending.erase(wms);
	} else {
		m_excess_gotblocks++;
	}
}

void RemoteClient::SendingBlock(const WantedMapSend &wms)
{
	if (m_blocks_sending.find(wms) == m_blocks_sending.end()) {
		warningstream<<"RemoteClient::SendingBlock(): Sent block"
				" already in m_blocks_sending"<<std::endl;
	}
	m_blocks_sending[wms] = time(NULL);
}

void RemoteClient::SetBlockUpdated(const WantedMapSend &wms)
{
	// Reset autosend's search radius but only if it's a MapBlock
	if (wms.type == WMST_MAPBLOCK) {
		m_autosend.resetSearchRadius();
	}

	m_blocks_updated_since_last_send.insert(wms);
}

void RemoteClient::SetMapBlockUpdated(v3s16 p)
{
	SetBlockUpdated(WantedMapSend(WMST_MAPBLOCK, p));

	// Also set the corresponding FarBlock not sent
	v3s16 farblock_p = getContainerPos(p, FMP_SCALE);
	SetBlockUpdated(WantedMapSend(WMST_FARBLOCK, farblock_p));

	/*dstream<<"RemoteClient: now not sent: MB"<<"("<<p.X<<","<<p.Y<<","<<p.Z<<")"
			<<" FB"<<"("<<farblock_p.X<<","<<farblock_p.Y<<","<<farblock_p.Z<<")"
			<<std::endl;*/
}

void RemoteClient::SetMapBlocksUpdated(std::map<v3s16, MapBlock*> &blocks)
{
	for(std::map<v3s16, MapBlock*>::iterator
			i = blocks.begin();
			i != blocks.end(); ++i)
	{
		v3s16 p = i->first;
		SetMapBlockUpdated(p);
	}
}

void RemoteClient::notifyEvent(ClientStateEvent event)
{
	std::ostringstream myerror;
	switch (m_state)
	{
	case CS_Invalid:
		//intentionally do nothing
		break;
	case CS_Created:
		switch (event) {
		case CSE_Hello:
			m_state = CS_HelloSent;
			break;
		case CSE_InitLegacy:
			m_state = CS_AwaitingInit2;
			break;
		case CSE_Disconnect:
			m_state = CS_Disconnecting;
			break;
		case CSE_SetDenied:
			m_state = CS_Denied;
			break;
		/* GotInit2 SetDefinitionsSent SetMediaSent */
		default:
			myerror << "Created: Invalid client state transition! " << event;
			throw ClientStateError(myerror.str());
		}
		break;
	case CS_Denied:
		/* don't do anything if in denied state */
		break;
	case CS_HelloSent:
		switch(event)
		{
		case CSE_AuthAccept:
			m_state = CS_AwaitingInit2;
			if ((chosen_mech == AUTH_MECHANISM_SRP)
					|| (chosen_mech == AUTH_MECHANISM_LEGACY_PASSWORD))
				srp_verifier_delete((SRPVerifier *) auth_data);
			chosen_mech = AUTH_MECHANISM_NONE;
			break;
		case CSE_Disconnect:
			m_state = CS_Disconnecting;
			break;
		case CSE_SetDenied:
			m_state = CS_Denied;
			if ((chosen_mech == AUTH_MECHANISM_SRP)
					|| (chosen_mech == AUTH_MECHANISM_LEGACY_PASSWORD))
				srp_verifier_delete((SRPVerifier *) auth_data);
			chosen_mech = AUTH_MECHANISM_NONE;
			break;
		default:
			myerror << "HelloSent: Invalid client state transition! " << event;
			throw ClientStateError(myerror.str());
		}
		break;
	case CS_AwaitingInit2:
		switch(event)
		{
		case CSE_GotInit2:
			confirmSerializationVersion();
			m_state = CS_InitDone;
			break;
		case CSE_Disconnect:
			m_state = CS_Disconnecting;
			break;
		case CSE_SetDenied:
			m_state = CS_Denied;
			break;

		/* Init SetDefinitionsSent SetMediaSent */
		default:
			myerror << "InitSent: Invalid client state transition! " << event;
			throw ClientStateError(myerror.str());
		}
		break;

	case CS_InitDone:
		switch(event)
		{
		case CSE_SetDefinitionsSent:
			m_state = CS_DefinitionsSent;
			break;
		case CSE_Disconnect:
			m_state = CS_Disconnecting;
			break;
		case CSE_SetDenied:
			m_state = CS_Denied;
			break;

		/* Init GotInit2 SetMediaSent */
		default:
			myerror << "InitDone: Invalid client state transition! " << event;
			throw ClientStateError(myerror.str());
		}
		break;
	case CS_DefinitionsSent:
		switch(event)
		{
		case CSE_SetClientReady:
			m_state = CS_Active;
			break;
		case CSE_Disconnect:
			m_state = CS_Disconnecting;
			break;
		case CSE_SetDenied:
			m_state = CS_Denied;
			break;
		/* Init GotInit2 SetDefinitionsSent */
		default:
			myerror << "DefinitionsSent: Invalid client state transition! " << event;
			throw ClientStateError(myerror.str());
		}
		break;
	case CS_Active:
		switch(event)
		{
		case CSE_SetDenied:
			m_state = CS_Denied;
			break;
		case CSE_Disconnect:
			m_state = CS_Disconnecting;
			break;
		case CSE_SudoSuccess:
			m_state = CS_SudoMode;
			if ((chosen_mech == AUTH_MECHANISM_SRP)
					|| (chosen_mech == AUTH_MECHANISM_LEGACY_PASSWORD))
				srp_verifier_delete((SRPVerifier *) auth_data);
			chosen_mech = AUTH_MECHANISM_NONE;
			break;
		/* Init GotInit2 SetDefinitionsSent SetMediaSent SetDenied */
		default:
			myerror << "Active: Invalid client state transition! " << event;
			throw ClientStateError(myerror.str());
			break;
		}
		break;
	case CS_SudoMode:
		switch(event)
		{
		case CSE_SetDenied:
			m_state = CS_Denied;
			break;
		case CSE_Disconnect:
			m_state = CS_Disconnecting;
			break;
		case CSE_SudoLeave:
			m_state = CS_Active;
			break;
		default:
			myerror << "Active: Invalid client state transition! " << event;
			throw ClientStateError(myerror.str());
			break;
		}
		break;
	case CS_Disconnecting:
		/* we are already disconnecting */
		break;
	}
}

u32 RemoteClient::uptime()
{
	return getTime(PRECISION_SECONDS) - m_connection_time;
}

ClientInterface::ClientInterface(con::Connection* con)
:
	m_con(con),
	m_env(NULL),
	m_print_info_timer(0.0)
{

}
ClientInterface::~ClientInterface()
{
	/*
		Delete clients
	*/
	{
		MutexAutoLock clientslock(m_clients_mutex);

		for(std::map<u16, RemoteClient*>::iterator
			i = m_clients.begin();
			i != m_clients.end(); ++i)
		{

			// Delete client
			delete i->second;
		}
	}
}

std::vector<u16> ClientInterface::getClientIDs(ClientState min_state)
{
	std::vector<u16> reply;
	MutexAutoLock clientslock(m_clients_mutex);

	for(std::map<u16, RemoteClient*>::iterator
		i = m_clients.begin();
		i != m_clients.end(); ++i)
	{
		if (i->second->getState() >= min_state)
			reply.push_back(i->second->peer_id);
	}

	return reply;
}

std::vector<std::string> ClientInterface::getPlayerNames()
{
	return m_clients_names;
}


void ClientInterface::step(float dtime)
{
	m_print_info_timer += dtime;
	if(m_print_info_timer >= 30.0)
	{
		m_print_info_timer = 0.0;
		UpdatePlayerList();
	}
}

void ClientInterface::UpdatePlayerList()
{
	if (m_env != NULL)
		{
		std::vector<u16> clients = getClientIDs();
		m_clients_names.clear();


		if(!clients.empty())
			infostream<<"Players:"<<std::endl;

		for(std::vector<u16>::iterator
			i = clients.begin();
			i != clients.end(); ++i) {
			Player *player = m_env->getPlayer(*i);

			if (player == NULL)
				continue;

			infostream << "* " << player->getName() << "\t";

			{
				MutexAutoLock clientslock(m_clients_mutex);
				RemoteClient* client = lockedGetClientNoEx(*i);
				if(client != NULL)
					client->PrintInfo(infostream);
			}

			m_clients_names.push_back(player->getName());
		}
	}
}

void ClientInterface::send(u16 peer_id, u8 channelnum,
		NetworkPacket* pkt, bool reliable)
{
	m_con->Send(peer_id, channelnum, pkt, reliable);
}

void ClientInterface::sendToAll(u16 channelnum,
		NetworkPacket* pkt, bool reliable)
{
	MutexAutoLock clientslock(m_clients_mutex);
	for(std::map<u16, RemoteClient*>::iterator
		i = m_clients.begin();
		i != m_clients.end(); ++i) {
		RemoteClient *client = i->second;

		if (client->net_proto_version != 0) {
			m_con->Send(client->peer_id, channelnum, pkt, reliable);
		}
	}
}

RemoteClient* ClientInterface::getClientNoEx(u16 peer_id, ClientState state_min)
{
	MutexAutoLock clientslock(m_clients_mutex);
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);
	// The client may not exist; clients are immediately removed if their
	// access is denied, and this event occurs later then.
	if(n == m_clients.end())
		return NULL;

	if (n->second->getState() >= state_min)
		return n->second;
	else
		return NULL;
}

RemoteClient* ClientInterface::lockedGetClientNoEx(u16 peer_id, ClientState state_min)
{
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);
	// The client may not exist; clients are immediately removed if their
	// access is denied, and this event occurs later then.
	if(n == m_clients.end())
		return NULL;

	if (n->second->getState() >= state_min)
		return n->second;
	else
		return NULL;
}

ClientState ClientInterface::getClientState(u16 peer_id)
{
	MutexAutoLock clientslock(m_clients_mutex);
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);
	// The client may not exist; clients are immediately removed if their
	// access is denied, and this event occurs later then.
	if(n == m_clients.end())
		return CS_Invalid;

	return n->second->getState();
}

void ClientInterface::setPlayerName(u16 peer_id,std::string name)
{
	MutexAutoLock clientslock(m_clients_mutex);
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);
	// The client may not exist; clients are immediately removed if their
	// access is denied, and this event occurs later then.
	if(n != m_clients.end())
		n->second->setName(name);
}

void ClientInterface::DeleteClient(u16 peer_id)
{
	MutexAutoLock conlock(m_clients_mutex);

	// Error check
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);
	// The client may not exist; clients are immediately removed if their
	// access is denied, and this event occurs later then.
	if(n == m_clients.end())
		return;

	/*
		Mark objects to be not known by the client
	*/
	//TODO this should be done by client destructor!!!
	RemoteClient *client = n->second;
	// Handle objects
	for(std::set<u16>::iterator
			i = client->m_known_objects.begin();
			i != client->m_known_objects.end(); ++i)
	{
		// Get object
		u16 id = *i;
		ServerActiveObject* obj = m_env->getActiveObject(id);

		if(obj && obj->m_known_by_count > 0)
			obj->m_known_by_count--;
	}

	// Delete client
	delete m_clients[peer_id];
	m_clients.erase(peer_id);
}

void ClientInterface::CreateClient(u16 peer_id)
{
	MutexAutoLock conlock(m_clients_mutex);

	// Error check
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);
	// The client shouldn't already exist
	if(n != m_clients.end()) return;

	// Create client
	RemoteClient *client = new RemoteClient(m_env);
	client->peer_id = peer_id;
	m_clients[client->peer_id] = client;
}

void ClientInterface::event(u16 peer_id, ClientStateEvent event)
{
	{
		MutexAutoLock clientlock(m_clients_mutex);

		// Error check
		std::map<u16, RemoteClient*>::iterator n;
		n = m_clients.find(peer_id);

		// No client to deliver event
		if (n == m_clients.end())
			return;
		n->second->notifyEvent(event);
	}

	if ((event == CSE_SetClientReady) ||
		(event == CSE_Disconnect)     ||
		(event == CSE_SetDenied))
	{
		UpdatePlayerList();
	}
}

u16 ClientInterface::getProtocolVersion(u16 peer_id)
{
	MutexAutoLock conlock(m_clients_mutex);

	// Error check
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);

	// No client to get version
	if (n == m_clients.end())
		return 0;

	return n->second->net_proto_version;
}

void ClientInterface::setClientVersion(u16 peer_id, u8 major, u8 minor, u8 patch, std::string full)
{
	MutexAutoLock conlock(m_clients_mutex);

	// Error check
	std::map<u16, RemoteClient*>::iterator n;
	n = m_clients.find(peer_id);

	// No client to set versions
	if (n == m_clients.end())
		return;

	n->second->setVersionInfo(major,minor,patch,full);
}
