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
		SetBlockNotSent(wms);
	}
}

void RemoteClient::GetNextBlocks (
		ServerEnvironment *env,
		EmergeManager *emerge,
		float dtime,
		std::vector<WantedMapSend> &dest)
{
	// If the client has not indicated it supports the new algorithm, fill in
	// autosend parameters and things should work fine
	if (m_fallback_autosend_active)
	{
		m_autosend_radius_map = g_settings->getS16("max_block_send_distance");
		m_autosend_radius_far = 0; // Old client does not understand FarBlocks

		// Continue normally.
	}

	/*
		Auto-send

		NOTE: All auto-sent stuff is considered higher priority than custom
		transfers. If the client wants to get custom stuff quickly, it has to
		disable autosend.
	*/
	if (m_autosend_radius_map > 0 || m_autosend_radius_far > 0) {
		GetNextAutosendBlocks(env, emerge, dtime, dest);
	}

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

			// Don't send blocks that have already been sent
			if (m_blocks_sent.find(wms) != m_blocks_sent.end())
				continue;

			// If the MapBlock is not loaded, it will be queued to be loaded or
			// generated. Otherwise it will be added to 'dest'.

			const bool generate_allowed = true;

			MapBlock *block = env->getMap().getBlockNoCreateNoEx(wms.p);

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
					infostream<<"Emerging MapBlock ("
							<<wms.p.X<<","<<wms.p.Y<<","<<wms.p.Z<<")"
							<<std::endl;
				}

				// This block is not available now; hopefully it appears on some
				// next call to this function.
				continue;
			}

			// The block is loaded; put it in dest so that if we're lucky, it
			// will be transferred to the client
			dest.push_back(wms);
		}
		if (wms.type == WMST_FARBLOCK) {
			verbosestream << "Server: Client "<<peer_id<<" wants FarBlock ("
					<<wms.p.X<<","<<wms.p.Y<<","<<wms.p.Z<<")"
					<< std::endl;

			// Do not go over-limit
			if (blockpos_over_limit(wms.p))
				continue;

			// Don't send blocks that are currently being transferred
			if (m_blocks_sending.find(wms) != m_blocks_sending.end())
				continue;

			// Don't send blocks that have already been sent
			if (m_blocks_sent.find(wms) != m_blocks_sent.end())
				continue;

			// TODO: Check if data for this is available and if not, possibly
			//       request an emerge of the required area

			// Put the block in dest so that if we're lucky, it will be
			// transferred to the client
			dest.push_back(wms);
		}
	}
}

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

void RemoteClient::GetNextAutosendBlocks (
		ServerEnvironment *env,
		EmergeManager *emerge,
		float dtime,
		std::vector<WantedMapSend> &dest)
{
	DSTACK(FUNCTION_NAME);

	// Increment timers
	m_nothing_sent_timer += dtime;
	m_nearest_unsent_reset_timer += dtime;
	m_nothing_to_send_pause_timer -= dtime;

	if (m_nothing_to_send_pause_timer >= 0)
		return;

	Player *player = env->getPlayer(peer_id);
	if (player == NULL) {
		// This can happen sometimes; clients and players are not in perfect sync.
		return;
	}

	// Won't send anything if already sending
	if (m_blocks_sending.size() >= g_settings->getU16
			("max_simultaneous_block_sends_per_client")) {
		//infostream<<"Not sending any blocks, Queue full."<<std::endl;
		return;
	}

	v3f camera_p = player->getEyePosition();
	v3f player_speed = player->getSpeed();

	// Get player position and figure out a good focus point for block selection
	v3f player_speed_dir(0,0,0);
	if(player_speed.getLength() > 1.0*BS)
		player_speed_dir = player_speed / player_speed.getLength();
	// Predict to next block
	v3f camera_p_predicted = camera_p + player_speed_dir*MAP_BLOCKSIZE*BS;
	v3s16 focus_point_nodepos = floatToInt(camera_p_predicted, BS);
	v3s16 focus_point = getNodeBlockPos(focus_point_nodepos);

	// Camera position and direction
	v3f camera_dir = v3f(0,0,1);
	camera_dir.rotateYZBy(player->getPitch());
	camera_dir.rotateXZBy(player->getYaw());

	// If focus point has changed to a different MapBlock, reset radius value
	// for iterating from zero again
	if (m_last_focus_point != focus_point) {
		m_nearest_unsent_d = 0;
		m_last_focus_point = focus_point;
	}

	// Get some settings
	const u16 max_simul_sends_setting =
			g_settings->getU16("max_simultaneous_block_sends_per_client");
	const float time_from_building_limit_s =
			g_settings->getFloat("full_block_send_enable_min_time_from_building");
	const s16 max_block_send_distance_setting =
			g_settings->getS16("max_block_send_distance");
	const s16 max_block_generate_distance =
			g_settings->getS16("max_block_generate_distance");

	// Derived settings
	const s16 max_block_send_distance =
			m_autosend_radius_map < max_block_send_distance_setting ?
			m_autosend_radius_map : max_block_send_distance_setting;

	// Number of blocks sending + number of blocks selected for sending
	u32 num_blocks_selected = m_blocks_sending.size();

	// Reset periodically to workaround possible glitches due to whatever
	// reasons (this is somewhat guided by just heuristics, after all)
	if (m_nearest_unsent_reset_timer > 20.0) {
		m_nearest_unsent_reset_timer = 0;
		m_nearest_unsent_d = 0;
	}

	// We will start from a radius that still has unsent MapBlocks
	s16 d_start = m_nearest_unsent_d >= 0 ? m_nearest_unsent_d : 0;

	//infostream<<"d_start="<<d_start<<std::endl;

	// Don't loop very much at a time. This function is called each server tick
	// so just a few steps will work fine (+2 is 3 steps per call).
	s16 d_max = 0;
	if (d_start < 5)
		d_max = d_start + 2;
	else if (d_max < 8)
		d_max = d_start + 1;
	else
		d_max = d_start; // These iterations start to be rather heavy

	if (d_max > max_block_send_distance)
		d_max = max_block_send_distance;

	// Keep track of... things. We need these in order to figure out where to
	// continue iterating on the next call to this function.
	s32 nearest_emergequeued_d = INT32_MAX;
	s32 nearest_emergefull_d = INT32_MAX;
	s32 nearest_sendqueued_d = INT32_MAX;

	// Out-of-FOV distance limit
	s16 fov_limit_activation_distance = m_fov_limit_enabled ?
			max_block_send_distance / 2 : max_block_send_distance;

	// TODO: Get FarBlocks

	s16 d; // Current radius in MapBlocks
	for(d = d_start; d <= d_max; d++) {
		/*
			Get the border/face dot coordinates of a "d-radiused"
			box
		*/
		std::vector<v3s16> list = FacePositionCache::getFacePositions(d);

		std::vector<v3s16>::iterator li;
		for(li = list.begin(); li != list.end(); ++li) {
			v3s16 p = *li + focus_point;
			WantedMapSend wms(WMST_MAPBLOCK, p);

			u16 max_simultaneous_block_sends =
					figure_out_max_simultaneous_block_sends(
							max_simul_sends_setting,
							m_time_from_building,
							time_from_building_limit_s,
							d);

			// Don't select too many blocks for sending
			if (num_blocks_selected >= max_simultaneous_block_sends) {
				goto queue_full_break;
			}

			// Don't send blocks that are currently being transferred
			if (m_blocks_sending.find(wms) != m_blocks_sending.end())
				continue;

			// Don't go over hard map limits
			if (blockpos_over_limit(p))
				continue;

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
				if(isBlockInSight(p, camera_p, camera_dir, m_autosend_fov,
						10000*BS) == false)
				{
					continue;
				}
			}

			// Don't send blocks that have already been sent
			if (m_blocks_sent.find(wms) != m_blocks_sent.end())
				continue;

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
				// get next one.
				continue;
			}

			/*
				Add inexistent block to emerge queue.
			*/
			if(block == NULL || surely_not_found_on_disk || emerge_required)
			{
				if (emerge->enqueueBlockEmerge(peer_id, p, generate_allowed)) {
					if (nearest_emergequeued_d == INT32_MAX)
						nearest_emergequeued_d = d;
				} else {
					if (nearest_emergefull_d == INT32_MAX)
						nearest_emergefull_d = d;
					goto queue_full_break;
				}

				// get next one.
				continue;
			}

			if(nearest_sendqueued_d == INT32_MAX)
				nearest_sendqueued_d = d;

			/*
				Add block to send queue
			*/

			dest.push_back(wms);

			num_blocks_selected += 1;
			m_nothing_sent_timer = 0.0f;
		}
	}
queue_full_break:

	/*infostream << "nearest_emergequeued_d = "<<nearest_emergequeued_d
			<<", nearest_emergefull_d = "<<nearest_emergefull_d
			<<", nearest_sendqueued_d = "<<nearest_sendqueued_d
			<<std::endl;*/

	// Because none of the things we queue for sending or emerging or anything
	// will necessarily be sent or anything, next time we need to continue
	// iterating from the closest radius of any of that happening so that we can
	// check whether they were sent or emerged or otherwise handled.
	s32 closest_required_re_check = INT32_MAX;
	if (nearest_emergequeued_d < closest_required_re_check)
		closest_required_re_check = nearest_emergequeued_d;
	if (nearest_emergefull_d < closest_required_re_check)
		closest_required_re_check = nearest_emergefull_d;
	if (nearest_sendqueued_d < closest_required_re_check)
		closest_required_re_check = nearest_sendqueued_d;

	if (closest_required_re_check != INT32_MAX) {
		// We did something that requires a result to be checked later. Continue
		// from that on the next time.
		m_nearest_unsent_d = closest_required_re_check;

		// If nothing has been sent in a moment, indicating that the emerge
		// thread is not finding anything on disk anymore, start a fresh pass
		// without the FOV limit.
		if (m_nothing_sent_timer >= 3.0f && m_autosend_fov != 0.0f &&
				m_fov_limit_enabled) {
			m_nearest_unsent_d = 0;
			m_fov_limit_enabled = false;
			// Have to be reset in order to not trigger this immediately again
			m_nothing_sent_timer = 0.0f;
		}
	} else if (d > max_block_send_distance) {
		// We iterated all the way through to the end of the send radius, if you
		// can believe that.
		if (m_autosend_fov != 0.0f && m_fov_limit_enabled) {
			// Do a second pass with FOV limiting disabled
			m_nearest_unsent_d = 0;
			m_fov_limit_enabled = false;
		} else {
			// Start from the beginning after a short idle delay, with FOV
			// limiting enabled because nobody knows what the future holds.
			m_nearest_unsent_d = 0;
			m_fov_limit_enabled = true;
			m_nothing_to_send_pause_timer = 2.0;
		}
	} else {
		// Absolutely nothing interesting happened. Next time we will continue
		// iterating from the next radius.
		m_nearest_unsent_d = d;
	}
}

void RemoteClient::GotBlock(const WantedMapSend &wms)
{
	if(m_blocks_sending.find(wms) != m_blocks_sending.end())
		m_blocks_sending.erase(wms);
	else
	{
		m_excess_gotblocks++;
	}
	m_blocks_sent.insert(wms);
}

void RemoteClient::SendingBlock(const WantedMapSend &wms)
{
	if(m_blocks_sending.find(wms) == m_blocks_sending.end())
		m_blocks_sending[wms] = 0.0;
	else
		infostream<<"RemoteClient::SendingBlock(): Sent block"
				" already in m_blocks_sending"<<std::endl;
}

void RemoteClient::SetBlockNotSent(const WantedMapSend &wms)
{
	m_nearest_unsent_d = 0;

	if(m_blocks_sending.find(wms) != m_blocks_sending.end())
		m_blocks_sending.erase(wms);
	if(m_blocks_sent.find(wms) != m_blocks_sent.end())
		m_blocks_sent.erase(wms);
}

void RemoteClient::SetMapBlockNotSent(v3s16 p)
{
	SetBlockNotSent(WantedMapSend(WMST_MAPBLOCK, p));

	// Also set the corresponding FarBlock not sent
	v3s16 farblock_p = getContainerPos(p, FMP_SCALE);
	SetBlockNotSent(WantedMapSend(WMST_FARBLOCK, farblock_p));
}

void RemoteClient::SetMapBlocksNotSent(std::map<v3s16, MapBlock*> &blocks)
{
	m_nearest_unsent_d = 0;

	for(std::map<v3s16, MapBlock*>::iterator
			i = blocks.begin();
			i != blocks.end(); ++i)
	{
		v3s16 p = i->first;
		SetMapBlockNotSent(p);
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
	RemoteClient *client = new RemoteClient();
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
