/*
 * Copyright (C) 2010 Bluecherry, LLC
 *
 * Confidential, all rights reserved. No distribution is permitted.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <sys/statvfs.h>
#include <bsd/string.h>

#include <libavutil/log.h>

#include "bc-server.h"

static BC_DECLARE_LIST(bc_rec_list);

static int max_threads;
static int cur_threads;
static int record_id = -1;

char global_sched[7 * 24 + 1];

static pthread_mutex_t media_lock = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;

struct bc_storage {
	char path[PATH_MAX];
	float min_thresh, max_thresh;
};

#define MAX_STOR_LOCS	10
static struct bc_storage media_stor[MAX_STOR_LOCS];

extern char *__progname;

/* Fake H.264 encoder for libavcodec. We're only muxing video, never reencoding,
 * so a real encoder isn't neeeded, but one must be present for the process to
 * succeed. ffmpeg does not support h264 encoding without libx264, which is GPL.
 */
static int fake_h264_init(AVCodecContext *ctx)
{
	return 0;
}

static int fake_h264_close(AVCodecContext *ctx)
{
	return 0;
}

static int fake_h264_frame(AVCodecContext *ctx, uint8_t *buf, int bufsize, void *data)
{
	return -1;
}

AVCodec fake_h264_encoder = {
	.name           = "fakeh264",
	.type           = AVMEDIA_TYPE_VIDEO,
	.id             = CODEC_ID_H264,
	.priv_data_size = 0,
	.init           = fake_h264_init,
	.encode         = fake_h264_frame,
	.close          = fake_h264_close,
	.capabilities   = CODEC_CAP_DELAY,
	.pix_fmts       = (const enum PixelFormat[]) { PIX_FMT_YUV420P, PIX_FMT_YUVJ420P, PIX_FMT_NONE },
	.long_name      = "Fake H.264 Encoder for RTP Muxing",
};

/* XXX Create a function here so that we don't have to do so many
 * SELECT's in bc_check_globals() */

/* Update our global settings */
static void bc_check_globals(void)
{
	BC_DB_RES dbres;
	int i;

	/* Get global schedule, default to continuous */
	dbres = bc_db_get_table("SELECT * from GlobalSettings WHERE "
				"parameter='G_DEV_SCED'");

	if (dbres != NULL && !bc_db_fetch_row(dbres)) {
		const char *sched = bc_db_get_val(dbres, "value", NULL);
		if (sched)
			strlcpy(global_sched, sched, sizeof(global_sched));
	} else {
		/* Default to continuous record */
		memset(global_sched, 'C', sizeof(global_sched));
		global_sched[sizeof(global_sched)-1] = 0;
	}
	bc_db_free_table(dbres);

	/* Get path to media storage locations, or use default */
	dbres = bc_db_get_table("SELECT * from Storage ORDER BY "
				"priority ASC");

	if (pthread_mutex_lock(&media_lock) == EDEADLK)
		bc_log("E: Deadlock detected in media_lock on db_check!");

	memset(media_stor, 0, sizeof(media_stor));
	i = 0;
	if (dbres != NULL) {
		while (!bc_db_fetch_row(dbres)) {
			const char *path = bc_db_get_val(dbres, "path", NULL);
			float max_thresh = bc_db_get_val_float(dbres, "max_thresh");
			float min_thresh = bc_db_get_val_float(dbres, "min_thresh");

			if (!path || !strlen(path) || max_thresh <= 0 ||
			    min_thresh <= 0)
				continue;

			strcpy(media_stor[i].path, path);
			media_stor[i].max_thresh = max_thresh;
			media_stor[i].min_thresh = min_thresh;

			bc_mkdir_recursive(media_stor[i].path);
			i++;
		}
	}
	if (i == 0) {
		/* Fall back to one single default location */
		bc_mkdir_recursive("/var/lib/bluecherry/recordings");
		strcpy(media_stor[0].path, "/var/lib/bluecherry/recordings");
		media_stor[0].max_thresh = 95.00;
		media_stor[i].min_thresh = 90.00;
	}

	pthread_mutex_unlock(&media_lock);

	bc_db_free_table(dbres);
}

static void bc_stop_threads(void)
{
	struct bc_record *bc_rec, *__t;
	char *errmsg = NULL;

	if (bc_list_empty(&bc_rec_list))
		return;

	bc_list_for_each_entry_safe(bc_rec, __t, &bc_rec_list, list)
		bc_rec->thread_should_die = "Shutting down";

	bc_list_for_each_entry_safe(bc_rec, __t, &bc_rec_list, list) {
		pthread_join(bc_rec->thread, (void **)&errmsg);
		bc_dev_info(bc_rec, "Camera thread stopped: %s", errmsg);
		bc_list_del(&bc_rec->list);
		bc_handle_free(bc_rec->bc);
		free(bc_rec);
		cur_threads--;
	}
}

/* Check for threads that have quit */
static void bc_check_threads(void)
{
	struct bc_record *bc_rec, *__t;
	char *errmsg = NULL;

	if (bc_list_empty(&bc_rec_list))
		return;

	bc_list_for_each_entry_safe(bc_rec, __t, &bc_rec_list, list) {
		if (pthread_tryjoin_np(bc_rec->thread, (void **)&errmsg))
			continue;

		bc_dev_info(bc_rec, "Camera thread stopped: %s", errmsg);
		bc_list_del(&bc_rec->list);
		bc_handle_free(bc_rec->bc);
		free(bc_rec);
		cur_threads--;
	}
}

static struct bc_record *bc_record_exists(const int id)
{
	struct bc_record *bc_rec;
	struct bc_list_struct *lh;

	if (bc_list_empty(&bc_rec_list))
		return NULL;

	bc_list_for_each(lh, &bc_rec_list) {
		bc_rec = bc_list_entry(lh, struct bc_record, list);
		if (bc_rec->id == id)
			return bc_rec;
	}

	return NULL;
}

static float storage_used(const struct bc_storage *stor)
{
	struct statvfs st;

	if (statvfs(stor->path, &st))
		return -1.00;

	return 100.0 - ((float)((float)st.f_bavail / (float)st.f_blocks) * 100);
}

static int storage_full(const struct bc_storage *stor)
{
	if (storage_used(stor) >= stor->max_thresh)
		return 1;

	return 0;
}

void bc_get_media_loc(char *stor)
{
	int i;

	stor[0] = 0;

	if (pthread_mutex_lock(&media_lock) == EDEADLK)
		bc_log("E: Deadlock detected in media_lock on get_loc!");

	for (i = 0; i < MAX_STOR_LOCS && media_stor[i].max_thresh; i++) {
		if (!storage_full(&media_stor[i])) {
			strcpy(stor, media_stor[i].path);
			break;
		}
	}

	if (stor[0] == 0)
		strcpy(stor, media_stor[0].path);

	pthread_mutex_unlock(&media_lock);
}


/* Check if our media directory is getting full (min_avail%) and delete old
 * events until there's more than or equal to min_thresh% available. Do not
 * delete archived events. If we've deleted everything we can and we still
 * don't have more than min_avail% available, then complain....LOUDLY! */
static void bc_clear_media_one(const struct bc_storage *stor)
{
	BC_DB_RES dbres;
	float used = storage_used(stor);

	if (used < stor->max_thresh)
		return;

	bc_log("I: Filesystem for %s is %0.2f%% full, starting cleanup",
	       stor->path, used);

	dbres = bc_db_get_table("SELECT * from Media WHERE archive=0 AND "
				"end!=0 AND size>0 AND filepath LIKE '%s%%' "
				"ORDER BY start ASC", stor->path);

	if (dbres == NULL) {
		bc_log("W: Filesystem has no available media to delete!");
		bc_event_sys(BC_EVENT_L_ALRM, BC_EVENT_SYS_T_DISK);
		return;
	}

	while (!bc_db_fetch_row(dbres) && used > stor->min_thresh) {
		const char *filepath = bc_db_get_val(dbres, "filepath", NULL);
		int id = bc_db_get_val_int(dbres, "id");

		if (filepath == NULL || !strlen(filepath))
			continue;

		unlink(filepath);
		__bc_db_query("UPDATE Media SET filepath='',size=0 "
			      "WHERE id=%d", id);

		bc_log("W: Removed media id %d, file '%s', to make space", id,
		       filepath);

		if ((used = storage_used(stor)) < 0)
			break;
        }

	bc_db_free_table(dbres);

	if (used < 0)
		return;

	if (used >= stor->min_thresh) {
		bc_log("W: Filesystem is %0.2f%% full, but cannot delete "
		       "any more old media!", used);
		bc_event_sys(BC_EVENT_L_ALRM, BC_EVENT_SYS_T_DISK);
	}
}

static void bc_check_media(void)
{
	int i, free = 0;

	if (pthread_mutex_lock(&media_lock) == EDEADLK)
		bc_log("E: Deadlock detected in media_lock on check_media!");

	for (i = 0; i < MAX_STOR_LOCS && media_stor[i].max_thresh; i++) {
		if (!storage_full(&media_stor[i])) {
			free = 1;
			break;
		}
	}

	if (!free) {
		for (i = 0; i < MAX_STOR_LOCS && media_stor[i].max_thresh; i++)
			bc_clear_media_one(&media_stor[i]);
	}

	pthread_mutex_unlock(&media_lock);
}

static void bc_check_db(void)
{
	struct bc_record *bc_rec;
	BC_DB_RES dbres;

	dbres = bc_db_get_table("SELECT * from Devices LEFT JOIN "
				"AvailableSources USING (device)");

	if (dbres == NULL)
		return;

	while (!bc_db_fetch_row(dbres)) {
		const char *proto = bc_db_get_val(dbres, "protocol", NULL);
		int id = bc_db_get_val_int(dbres, "id");

		if ((id < 0) || !proto)
			continue;

		bc_rec = bc_record_exists(id);
		if (bc_rec) {
			bc_record_update_cfg(bc_rec, dbres);
			continue;
		}

		/* Caller asked us to only use this record_id */
		if (record_id >= 0 && id != record_id)
			continue;

		/* Caller asked us to only start so many threads */
		if (max_threads && cur_threads >= max_threads)
			continue;

		/* If this is a V4L2 device, it needs to be detected */
		if (!strcasecmp(proto, "V4L2")) {
			int card_id = bc_db_get_val_int(dbres, "card_id");
			if (card_id < 0)
				continue;
		}

		bc_rec = bc_alloc_record(id, dbres);
		if (bc_rec == NULL)
			continue;

		cur_threads++;
		bc_list_add(&bc_rec->list, &bc_rec_list);
	}

	bc_db_free_table(dbres);
}

static void bc_check_inprogress(void)
{
	BC_DB_RES dbres;

	dbres = bc_db_get_table("SELECT EventsCam.id, EventsCam.media_id, "
				"Media.filepath FROM EventsCam LEFT JOIN "
				"Media ON (EventsCam.media_id=Media.id) "
				"WHERE length=-1");

	if (dbres == NULL)
		return;

	while (!bc_db_fetch_row(dbres)) {
		const char *filepath = bc_db_get_val(dbres, "filepath", NULL);
		int duration = 0;
		char cmd[4096];
		char *line = NULL;
		size_t len;
		FILE *fp;

		if (!filepath)
			continue;

		sprintf(cmd, "mkvinfo -v %s", filepath);

		fp = popen(cmd, "r");
		if (fp == NULL)
			continue;

		while (getline(&line, &len, fp) >= 0) {
			char *p;
			if ((p = strstr(line, "timecode ")))
				sscanf(p, "timecode %d.", &duration);
			free(line);
			line = NULL;
		}

		if (!duration) {
			unsigned int e_id, m_id;

			e_id = bc_db_get_val_int(dbres, "id");
			m_id = bc_db_get_val_int(dbres, "media_id");

			bc_log("Media %s has zero time so deleting",
			       filepath);

			bc_db_query("DELETE FROM EventsCam WHERE id=%u", e_id);
			bc_db_query("DELETE FROM Media WHERE id=%u", m_id);

			unlink(filepath);
		} else {
			unsigned int id = bc_db_get_val_int(dbres, "id");

			bc_log("Media %s left in-progress so updating length "
			       "to %d", filepath, duration);
			bc_db_query("UPDATE EventsCam SET length=%d WHERE "
				    "id=%d", duration, id);
		}

		pclose(fp);
        }

	bc_db_free_table(dbres);
}

static void usage(void)
{
	fprintf(stderr, "Usage: %s [-s]\n", __progname);
	fprintf(stderr, "  -s\tDo not background\n");
	fprintf(stderr, "  -m\tMax threads to start\n");
	fprintf(stderr, "  -r\tRecord a specific ID only\n");

	exit(1);
}

static pthread_key_t av_log_current_handle_key;
static const int av_log_without_handle = AV_LOG_INFO;

void bc_av_log_set_handle_thread(struct bc_record *bc_rec)
{
	pthread_setspecific(av_log_current_handle_key, bc_rec);
}

/* Warning: Must be reentrant; this may be called from many device threads at once */
static void av_log_cb(void *avcl, int level, const char *fmt, va_list ap)
{
	char msg[strlen(fmt) + 200];
	const char *levelstr;
	struct bc_record *bc_rec = (struct bc_record*)pthread_getspecific(av_log_current_handle_key);

	switch (level) {
		case AV_LOG_PANIC: levelstr = "PANIC"; break;
		case AV_LOG_FATAL: levelstr = "fatal"; break;
		case AV_LOG_ERROR: levelstr = "error"; break;
		case AV_LOG_WARNING: levelstr = "warning"; break;
		case AV_LOG_INFO: levelstr = "info"; break;
		case AV_LOG_VERBOSE: levelstr = "verbose"; break;
		case AV_LOG_DEBUG: levelstr = "debug"; break;
		default: levelstr = "???"; break;
	}

	if (!bc_rec) {
		if (level <= av_log_without_handle) {
			sprintf(msg, "[avlib %s]: %s", levelstr, fmt);
			bc_vlog(msg, ap);
		}
		return;
	}

	if ((bc_rec->cfg.debug_level < 0 && level > AV_LOG_FATAL) ||
	    (bc_rec->cfg.debug_level == 0 && level > AV_LOG_ERROR) ||
	    (bc_rec->cfg.debug_level == 1 && level > AV_LOG_INFO))
		return;

	sprintf(msg, "I(%d/%s): avlib %s: %s", bc_rec->id, bc_rec->cfg.name, levelstr, fmt);
	bc_vlog(msg, ap);
}

static void check_expire(void)
{
	time_t t = time(NULL);
	time_t expire = 1325116800; /* December 29, 2011 */

	if (t < expire)
		return;

	fprintf(stderr, "This beta expired on %s", ctime(&expire));
	exit(1);
}

int main(int argc, char **argv)
{
	int opt;
	unsigned int loops;
	int bg = 1;
	int count;

	check_expire();

	while ((opt = getopt(argc, argv, "hsm:r:")) != -1) {
		switch (opt) {
		case 's': bg = 0; break;
		case 'm': max_threads = atoi(optarg); break;
		case 'r': record_id = atoi(optarg); break;
		case 'h': default: usage();
		}
	}

	if (av_lockmgr_register(bc_av_lockmgr)) {
		bc_log("E: AV lock registration failed: %m");
		exit(1);
	}

	avcodec_init();
	avcodec_register(&fake_h264_encoder);
	av_register_all();

	pthread_key_create(&av_log_current_handle_key, NULL);
	av_log_set_callback(av_log_cb);

	if (bg && daemon(0, 0) == -1) {
		bc_log("E: Could not fork to background: %m");
		exit(1);
	}

	bc_log("I: Started Bluecherry daemon");

	for (count = 1; bc_db_open(); count++) {
		sleep(1);
		if (count % 30)
			continue;
		bc_log("E: Could not open SQL database after 30 seconds...");
	}

	bc_log("I: SQL database connection opened");

	/* Set these from the start */
	bc_check_globals();

	/* Do some cleanup */
	bc_check_inprogress();

	/* Main loop */
	for (loops = 0 ;; loops++) {
		/* Every 2 minutes */
		if (!(loops % 120)) {
			/* Check for new devices */
			bc_check_avail();
			/* Check media locations for full */
			bc_check_media();
		}

		/* Every 10 seconds */
		if (!(loops % 10)) {
			/* Check global vars */
			bc_check_globals();
			/* Check for changes in cameras */
			bc_check_db();
		}

		/* Every second, check for dead threads */
		bc_check_threads();
		/* And resolve un-committed events/media */
		bc_media_event_clear();

		sleep(1);
	}

	bc_stop_threads();
	bc_db_close();
	av_lockmgr_register(NULL);

	exit(0);
}
