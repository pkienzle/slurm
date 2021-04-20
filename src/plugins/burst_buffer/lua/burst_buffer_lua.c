/*****************************************************************************\
 *  burst_buffer_lua.c - Plugin for managing burst buffers with lua
 *****************************************************************************
 *  Copyright (C) 2021 SchedMD LLC.
 *  Written by Marshall Garey <marshall@schedmd.com>
 *
 *  This file is part of Slurm, a resource management program.
 *  For details, see <https://slurm.schedmd.com/>.
 *  Please also read the included file: DISCLAIMER.
 *
 *  Slurm is free software; you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation; either version 2 of the License, or (at your option)
 *  any later version.
 *
 *  In addition, as a special exception, the copyright holders give permission
 *  to link the code of portions of this program with the OpenSSL library under
 *  certain conditions as described in each individual source file, and
 *  distribute linked combinations including the two. You must obey the GNU
 *  General Public License in all respects for all of the code used other than
 *  OpenSSL. If you modify file(s) with this exception, you may extend this
 *  exception to your version of the file(s), but you are not obligated to do
 *  so. If you do not wish to do so, delete this exception statement from your
 *  version.  If you delete this exception statement from all source files in
 *  the program, then also delete it here.
 *
 *  Slurm is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Slurm; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
\*****************************************************************************/

#include "config.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdlib.h>
#include <unistd.h>

#include "slurm/slurm.h"

#include "src/common/assoc_mgr.h"
#include "src/common/fd.h"
#include "src/common/xstring.h"
#include "src/lua/slurm_lua.h"
#include "src/slurmctld/locks.h"
#include "src/slurmctld/slurmctld.h"
#include "src/plugins/burst_buffer/common/burst_buffer_common.h"

/*
 * These variables are required by the burst buffer plugin interface.  If they
 * are not found in the plugin, the plugin loader will ignore it.
 *
 * plugin_name - a string giving a human-readable description of the
 * plugin.  There is no maximum length, but the symbol must refer to
 * a valid string.
 *
 * plugin_type - a string suggesting the type of the plugin or its
 * applicability to a particular form of data or method of data handling.
 * If the low-level plugin API is used, the contents of this string are
 * unimportant and may be anything.  Slurm uses the higher-level plugin
 * interface which requires this string to be of the form
 *
 *      <application>/<method>
 *
 * where <application> is a description of the intended application of
 * the plugin (e.g., "burst_buffer" for Slurm burst_buffer) and <method> is a
 * description of how this plugin satisfies that application.  Slurm will only
 * load a burst_buffer plugin if the plugin_type string has a prefix of
 * "burst_buffer/".
 *
 * plugin_version - an unsigned 32-bit integer containing the Slurm version
 * (major.minor.micro combined into a single number).
 */
const char plugin_name[]        = "burst_buffer lua plugin";
const char plugin_type[]        = "burst_buffer/lua";
const uint32_t plugin_version   = SLURM_VERSION_NUMBER;

/*
 * Most state information is in a common structure so that we can more
 * easily use common functions from multiple burst buffer plugins.
 */
static bb_state_t bb_state;

static const char lua_script_path[] = DEFAULT_SCRIPT_DIR "/burst_buffer.lua";
static time_t lua_script_last_loaded = (time_t) 0;
static lua_State *L = NULL;
static const char *req_fxns[] = {
	"slurm_bb_job_process",
	NULL
};

static void _loadscript_extra(lua_State *st)
{
        /* local setup */
	/*
	 * We may add functions later (like job_submit/lua and cli_filter/lua),
	 * but for now we don't have any.
	 */
	//slurm_lua_table_register(st, NULL, slurm_functions);

	/* Must be always done after we register the slurm_functions */
	lua_setglobal(st, "slurm");
}

/*
 * Call a function in burst_buffer.lua.
 */
static int _run_lua_script(const char *lua_func)
{
	int rc;

	rc = slurm_lua_loadscript(&L, "burst_buffer/lua",
				  lua_script_path, req_fxns,
				  &lua_script_last_loaded, _loadscript_extra);

	if (rc != SLURM_SUCCESS)
		return rc;

	/*
	 * All lua script functions should have been verified during
	 * initialization:
	 */
	lua_getglobal(L, lua_func);
	if (lua_isnil(L, -1)) {
		error("%s: Couldn't find function %s",
		      __func__, lua_func);
		lua_close(L);
		return SLURM_ERROR;
	}

	slurm_lua_stack_dump("burst_buffer/lua", "before lua_pcall", L);
	if (lua_pcall(L, 0, 1, 0) != 0) {
		error("%s: %s",
		      lua_script_path, lua_tostring(L, -1));
	} else {
		if (lua_isnumber(L, -1)) {
			rc = lua_tonumber(L, -1);
		} else {
			info("%s: non-numeric return code, returning success",
			     lua_script_path);
			rc = SLURM_SUCCESS;
		}
		lua_pop(L, 1);
	}
	slurm_lua_stack_dump("burst_buffer/lua", "after lua_pcall", L);

	lua_close(L);
	return rc;
}

/*
 * Handle timeout of burst buffer events:
 * 1. Purge per-job burst buffer records when the stage-out has completed and
 *    the job has been purged from Slurm
 * 2. Test for StageInTimeout events
 * 3. Test for StageOutTimeout events
 */
static void _timeout_bb_rec(void)
{
	/* Not implemented yet. */
}

/*
 * Write current burst buffer state to a file.
 */
static void _save_bb_state(void)
{
	static time_t last_save_time = 0;
	static int high_buffer_size = 16 * 1024;
	time_t save_time = time(NULL);
	bb_alloc_t *bb_alloc;
	uint32_t rec_count = 0;
	buf_t *buffer;
	char *old_file = NULL, *new_file = NULL, *reg_file = NULL;
	int i, count_offset, offset, state_fd;
	int error_code = 0;
	uint16_t protocol_version = SLURM_PROTOCOL_VERSION;

	if ((bb_state.last_update_time <= last_save_time) &&
	    !bb_state.term_flag)
		return;

	buffer = init_buf(high_buffer_size);
	pack16(protocol_version, buffer);
	count_offset = get_buf_offset(buffer);
	pack32(rec_count, buffer);
	/* Each allocated burst buffer is in bb_state.bb_ahash */
	if (bb_state.bb_ahash) {
		slurm_mutex_lock(&bb_state.bb_mutex);
		for (i = 0; i < BB_HASH_SIZE; i++) {
			bb_alloc = bb_state.bb_ahash[i];
			while (bb_alloc) {
				packstr(bb_alloc->account, buffer);
				pack_time(bb_alloc->create_time, buffer);
				pack32(bb_alloc->id, buffer);
				packstr(bb_alloc->name, buffer);
				packstr(bb_alloc->partition, buffer);
				packstr(bb_alloc->pool, buffer);
				packstr(bb_alloc->qos, buffer);
				pack32(bb_alloc->user_id, buffer);
				pack64(bb_alloc->size,	buffer);
				rec_count++;
				bb_alloc = bb_alloc->next;
			}
		}
		save_time = time(NULL);
		slurm_mutex_unlock(&bb_state.bb_mutex);
		offset = get_buf_offset(buffer);
		set_buf_offset(buffer, count_offset);
		pack32(rec_count, buffer);
		set_buf_offset(buffer, offset);
	}

	xstrfmtcat(old_file, "%s/%s", slurm_conf.state_save_location,
	           "burst_buffer_lua_state.old");
	xstrfmtcat(reg_file, "%s/%s", slurm_conf.state_save_location,
	           "burst_buffer_lua_state");
	xstrfmtcat(new_file, "%s/%s", slurm_conf.state_save_location,
	           "burst_buffer_lua_state.new");

	state_fd = creat(new_file, 0600);
	if (state_fd < 0) {
		error("Can't save state, error creating file %s, %m",
		      new_file);
		error_code = errno;
	} else {
		int pos = 0, nwrite = get_buf_offset(buffer), amount, rc;
		char *data = (char *)get_buf_data(buffer);
		high_buffer_size = MAX(nwrite, high_buffer_size);
		while (nwrite > 0) {
			amount = write(state_fd, &data[pos], nwrite);
			if ((amount < 0) && (errno != EINTR)) {
				error("Error writing file %s, %m", new_file);
				break;
			}
			nwrite -= amount;
			pos    += amount;
		}

		rc = fsync_and_close(state_fd, "burst_buffer_lua");
		if (rc && !error_code)
			error_code = rc;
	}
	if (error_code)
		(void) unlink(new_file);
	else {
		/* file shuffle */
		last_save_time = save_time;
		(void) unlink(old_file);
		if (link(reg_file, old_file)) {
			debug4("unable to create link for %s -> %s: %m",
			       reg_file, old_file);
		}
		(void) unlink(reg_file);
		if (link(new_file, reg_file)) {
			debug4("unable to create link for %s -> %s: %m",
			       new_file, reg_file);
		}
		(void) unlink(new_file);
	}
	xfree(old_file);
	xfree(reg_file);
	xfree(new_file);
	free_buf(buffer);
}

static void _recover_bb_state(void)
{
	char *state_file = NULL, *data = NULL;
	int data_allocated, data_read = 0;
	uint16_t protocol_version = NO_VAL16;
	uint32_t data_size = 0, rec_count = 0, name_len = 0;
	uint32_t id = 0, user_id = 0;
	uint64_t size = 0;
	int i, state_fd;
	char *account = NULL, *name = NULL;
	char *partition = NULL, *pool = NULL, *qos = NULL;
	char *end_ptr = NULL;
	time_t create_time = 0;
	bb_alloc_t *bb_alloc;
	buf_t *buffer;

	state_fd = bb_open_state_file("burst_buffer_lua_state", &state_file);
	if (state_fd < 0) {
		info("No burst buffer state file (%s) to recover",
		     state_file);
		xfree(state_file);
		return;
	}
	data_allocated = BUF_SIZE;
	data = xmalloc(data_allocated);
	while (1) {
		data_read = read(state_fd, &data[data_size], BUF_SIZE);
		if (data_read < 0) {
			if  (errno == EINTR)
				continue;
			else {
				error("Read error on %s: %m", state_file);
				break;
			}
		} else if (data_read == 0)     /* eof */
			break;
		data_size      += data_read;
		data_allocated += data_read;
		xrealloc(data, data_allocated);
	}
	close(state_fd);
	xfree(state_file);

	buffer = create_buf(data, data_size);
	safe_unpack16(&protocol_version, buffer);
	if (protocol_version == NO_VAL16) {
		if (!ignore_state_errors)
			fatal("Can not recover burst_buffer/datawarp state, data version incompatible, start with '-i' to ignore this. Warning: using -i will lose the data that can't be recovered.");
		error("**********************************************************************");
		error("Can not recover burst_buffer/datawarp state, data version incompatible");
		error("**********************************************************************");
		return;
	}

	safe_unpack32(&rec_count, buffer);
	for (i = 0; i < rec_count; i++) {
		if (protocol_version >= SLURM_21_08_PROTOCOL_VERSION) {
			safe_unpackstr_xmalloc(&account,   &name_len, buffer);
			safe_unpack_time(&create_time, buffer);
			safe_unpack32(&id, buffer);
			safe_unpackstr_xmalloc(&name,      &name_len, buffer);
			safe_unpackstr_xmalloc(&partition, &name_len, buffer);
			safe_unpackstr_xmalloc(&pool,      &name_len, buffer);
			safe_unpackstr_xmalloc(&qos,       &name_len, buffer);
			safe_unpack32(&user_id, buffer);
			safe_unpack64(&size, buffer);
		}

		bb_alloc = bb_alloc_name_rec(&bb_state, name, user_id);
		bb_alloc->id = id;
		if (name && (name[0] >='0') && (name[0] <='9')) {
			bb_alloc->job_id = strtol(name, &end_ptr, 10);
			bb_alloc->array_job_id = bb_alloc->job_id;
			bb_alloc->array_task_id = NO_VAL;
		}
		bb_alloc->seen_time = time(NULL);
		bb_alloc->size = size;
		if (bb_alloc) {
			log_flag(BURST_BUF, "Recovered burst buffer %s from user %u",
				 bb_alloc->name, bb_alloc->user_id);
			xfree(bb_alloc->account);
			bb_alloc->account = account;
			account = NULL;
			bb_alloc->create_time = create_time;
			xfree(bb_alloc->partition);
			bb_alloc->partition = partition;
			partition = NULL;
			xfree(bb_alloc->pool);
			bb_alloc->pool = pool;
			pool = NULL;
			xfree(bb_alloc->qos);
			bb_alloc->qos = qos;
			qos = NULL;
		}
		xfree(account);
		xfree(name);
		xfree(partition);
		xfree(pool);
		xfree(qos);
	}

	info("Recovered state of %d burst buffers", rec_count);
	free_buf(buffer);
	return;

unpack_error:
	if (!ignore_state_errors)
		fatal("Incomplete burst buffer data checkpoint file, start with '-i' to ignore this. Warning: using -i will lose the data that can't be recovered.");
	error("Incomplete burst buffer data checkpoint file");
	xfree(account);
	xfree(name);
	xfree(partition);
	xfree(qos);
	free_buf(buffer);
	return;
}

static void _apply_limits(void)
{
	/* Not yet implemented */
}

static void _load_state(bool init_config)
{
	_recover_bb_state();
	_apply_limits();
	bb_state.last_update_time = time(NULL);

	return;
}

/* Perform periodic background activities */
static void *_bb_agent(void *args)
{
	/* Locks: write job */
	slurmctld_lock_t job_write_lock = {
		NO_LOCK, WRITE_LOCK, NO_LOCK, NO_LOCK, NO_LOCK };

	while (!bb_state.term_flag) {
		bb_sleep(&bb_state, AGENT_INTERVAL);
		if (!bb_state.term_flag) {
			_load_state(false);	/* Has own locking */
			lock_slurmctld(job_write_lock);
			slurm_mutex_lock(&bb_state.bb_mutex);
			_timeout_bb_rec();
			slurm_mutex_unlock(&bb_state.bb_mutex);
			unlock_slurmctld(job_write_lock);
		}
		_save_bb_state();	/* Has own locks excluding file write */
	}

	return NULL;
}

/*
 * init() is called when the plugin is loaded, before any other functions
 * are called.  Put global initialization here.
 */
extern int init(void)
{
	slurm_mutex_init(&bb_state.bb_mutex);
	slurm_mutex_lock(&bb_state.bb_mutex);
	bb_load_config(&bb_state, (char *)plugin_type); /* Removes "const" */
	log_flag(BURST_BUF, "");
	bb_alloc_cache(&bb_state);
	slurm_thread_create(&bb_state.bb_thread, _bb_agent, NULL);
	slurm_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * fini() is called when the plugin is unloaded. Free all memory.
 */
extern int fini(void)
{
	slurm_mutex_lock(&bb_state.bb_mutex);
	log_flag(BURST_BUF, "");

	slurm_mutex_lock(&bb_state.term_mutex);
	bb_state.term_flag = true;
	slurm_cond_signal(&bb_state.term_cond);
	slurm_mutex_unlock(&bb_state.term_mutex);

	if (bb_state.bb_thread) {
		slurm_mutex_unlock(&bb_state.bb_mutex);
		pthread_join(bb_state.bb_thread, NULL);
		slurm_mutex_lock(&bb_state.bb_mutex);
		bb_state.bb_thread = 0;
	}
	bb_clear_config(&bb_state.bb_config, true);
	bb_clear_cache(&bb_state);
	slurm_mutex_unlock(&bb_state.bb_mutex);

	return SLURM_SUCCESS;
}

/*
 * Return the total burst buffer size in MB
 */
extern uint64_t bb_p_get_system_size(void)
{
	uint64_t size = 0;
	return size;
}

/*
 * Load the current burst buffer state (e.g. how much space is available now).
 * Run at the beginning of each scheduling cycle in order to recognize external
 * changes to the burst buffer state (e.g. capacity is added, removed, fails,
 * etc.)
 *
 * init_config IN - true if called as part of slurmctld initialization
 * Returns a Slurm errno.
 */
extern int bb_p_load_state(bool init_config)
{
	return SLURM_SUCCESS;
}

/*
 * Return string containing current burst buffer status
 * argc IN - count of status command arguments
 * argv IN - status command arguments
 * RET status string, release memory using xfree()
 */
extern char *bb_p_get_status(uint32_t argc, char **argv)
{
	return NULL;
}

/*
 * Note configuration may have changed. Handle changes in BurstBufferParameters.
 *
 * Returns a Slurm errno.
 */
extern int bb_p_reconfig(void)
{
	return SLURM_SUCCESS;
}

/*
 * Pack current burst buffer state information for network transmission to
 * user (e.g. "scontrol show burst")
 *
 * Returns a Slurm errno.
 */
extern int bb_p_state_pack(uid_t uid, buf_t *buffer, uint16_t protocol_version)
{
	return SLURM_SUCCESS;
}

/*
 * Preliminary validation of a job submit request with respect to burst buffer
 * options. Performed after setting default account + qos, but prior to
 * establishing job ID or creating script file.
 *
 * Returns a Slurm errno.
 */
extern int bb_p_job_validate(job_desc_msg_t *job_desc, uid_t submit_uid)
{
	return SLURM_SUCCESS;
}

/*
 * Secondary validation of a job submit request with respect to burst buffer
 * options. Performed after establishing job ID and creating script file.
 *
 * Returns a Slurm errno.
 */
extern int bb_p_job_validate2(job_record_t *job_ptr, char **err_msg)
{
	int rc;

	log_flag(BURST_BUF, "%pJ", job_ptr);
	/* Run "job_process" function, validates user script */
	rc = _run_lua_script("slurm_bb_job_process");
	log_flag(BURST_BUF, "Return code=%d", rc);
	return rc;
}

/*
 * Fill in the tres_cnt (in MB) based off the job record
 * NOTE: Based upon job-specific burst buffers, excludes persistent buffers
 * IN job_ptr - job record
 * IN/OUT tres_cnt - fill in this already allocated array with tres_cnts
 * IN locked - if the assoc_mgr tres read locked is locked or not
 */
extern void bb_p_job_set_tres_cnt(job_record_t *job_ptr, uint64_t *tres_cnt,
				  bool locked)
{
}

/*
 * For a given job, return our best guess if when it might be able to start
 */
extern time_t bb_p_job_get_est_start(job_record_t *job_ptr)
{
	time_t est_start = time(NULL);
	return est_start;
}

/*
 * Attempt to allocate resources and begin file staging for pending jobs.
 */
extern int bb_p_job_try_stage_in(List job_queue)
{
	return SLURM_SUCCESS;
}

/*
 * Determine if a job's burst buffer stage-in is complete
 * job_ptr IN - Job to test
 * test_only IN - If false, then attempt to allocate burst buffer if possible
 *
 * RET: 0 - stage-in is underway
 *      1 - stage-in complete
 *     -1 - stage-in not started or burst buffer in some unexpected state
 */
extern int bb_p_job_test_stage_in(job_record_t *job_ptr, bool test_only)
{
	return 1;
}

/* Attempt to claim burst buffer resources.
 * At this time, bb_g_job_test_stage_in() should have been run successfully AND
 * the compute nodes selected for the job.
 *
 * Returns a Slurm errno.
 */
extern int bb_p_job_begin(job_record_t *job_ptr)
{
	return SLURM_SUCCESS;
}

/* Revoke allocation, but do not release resources.
 * Executed after bb_p_job_begin() if there was an allocation failure.
 * Does not release previously allocated resources.
 *
 * Returns a Slurm errno.
 */
extern int bb_p_job_revoke_alloc(job_record_t *job_ptr)
{
	return SLURM_SUCCESS;
}

/*
 * Trigger a job's burst buffer stage-out to begin
 *
 * Returns a Slurm errno.
 */
extern int bb_p_job_start_stage_out(job_record_t *job_ptr)
{
	return SLURM_SUCCESS;
}

/*
 * Determine if a job's burst buffer post_run operation is complete
 *
 * RET: 0 - post_run is underway
 *      1 - post_run complete
 *     -1 - fatal error
 */
extern int bb_p_job_test_post_run(job_record_t *job_ptr)
{
	return 1;
}

/*
 * Determine if a job's burst buffer stage-out is complete
 *
 * RET: 0 - stage-out is underway
 *      1 - stage-out complete
 *     -1 - fatal error
 */
extern int bb_p_job_test_stage_out(job_record_t *job_ptr)
{
	return 1;
}

/*
 * Terminate any file staging and completely release burst buffer resources
 *
 * Returns a Slurm errno.
 */
extern int bb_p_job_cancel(job_record_t *job_ptr)
{
	return SLURM_SUCCESS;
}

/*
 * Translate a burst buffer string to it's equivalent TRES string
 * Caller must xfree the return value
 */
extern char *bb_p_xlate_bb_2_tres_str(char *burst_buffer)
{
	return NULL;
}
