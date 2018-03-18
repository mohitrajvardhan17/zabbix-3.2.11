/*
** Zabbix
** Copyright (C) 2001-2017 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"
#include "daemon.h"
#include "zbxself.h"
#include "log.h"
#include "db.h"
#include "dbcache.h"
#include "../events.h"

#define ZBX_TASKMANAGER_TIMEOUT	5

extern unsigned char	process_type, program_type;
extern int		server_num, process_num;

/******************************************************************************
 *                                                                            *
 * Function: tm_execute_task_close_problem                                    *
 *                                                                            *
 * Purpose: close the specified problem event and remove task                 *
 *                                                                            *
 * Parameters: triggerid         - [IN] the source trigger id                 *
 *             eventid           - [IN] the problem eventid to close          *
 *             userid            - [IN] the user that requested to close the  *
 *                                      problem                               *
 *                                                                            *
 ******************************************************************************/
static void	tm_execute_task_close_problem(zbx_uint64_t triggerid, zbx_uint64_t eventid, zbx_uint64_t userid)
{
	const char	*__function_name = "tm_execute_task_close_problem";

	DB_RESULT	result;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() eventid:" ZBX_FS_UI64, __function_name, eventid);

	result = DBselect("select null from problem where eventid=" ZBX_FS_UI64 " and r_eventid is null", eventid);

	/* check if the task hasn't been already closed by another process */
	if (NULL != DBfetch(result))
		zbx_close_problem(triggerid, eventid, userid);

	DBfree_result(result);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: tm_try_task_close_problem                                        *
 *                                                                            *
 * Purpose: try to close problem by event acknowledgment action               *
 *                                                                            *
 * Parameters: taskid - [IN] the task identifier                              *
 *                                                                            *
 * Return value: SUCCEED - task was executed and removed                      *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	tm_try_task_close_problem(zbx_uint64_t taskid)
{
	const char		*__function_name = "tm_try_task_close_problem";

	DB_ROW			row;
	DB_RESULT		result;
	int			ret = FAIL, remove_task = 0;
	zbx_uint64_t		userid, triggerid, eventid;
	zbx_vector_uint64_t	triggerids, locked_triggerids;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() taskid:" ZBX_FS_UI64, __function_name, taskid);

	zbx_vector_uint64_create(&triggerids);
	zbx_vector_uint64_create(&locked_triggerids);

	result = DBselect("select a.userid,e.eventid,e.objectid"
				" from task_close_problem tcp"
				" left join acknowledges a"
					" on a.acknowledgeid=tcp.acknowledgeid"
				" left join events e"
					" on a.eventid=e.eventid"
				" where tcp.taskid=" ZBX_FS_UI64,
			taskid);

	if (NULL != (row = DBfetch(result)))
	{
		if (SUCCEED != DBis_null(row[1]))
		{
			ZBX_STR2UINT64(triggerid, row[2]);
			zbx_vector_uint64_append(&triggerids, triggerid);
			DCconfig_lock_triggers_by_triggerids(&triggerids, &locked_triggerids);

			/* only close the problem if source trigger was successfully locked */
			if (0 != locked_triggerids.values_num)
			{
				ZBX_STR2UINT64(userid, row[0]);
				ZBX_STR2UINT64(eventid, row[1]);

				tm_execute_task_close_problem(triggerid, eventid, userid);
				remove_task = 1;

				ret = SUCCEED;
			}
		}
		else
			remove_task = 1;
	}
	DBfree_result(result);

	/* remove the task if it was executed or related event was deleted before task was processed */
	if (1 == remove_task)
		DBexecute("delete from task where taskid=" ZBX_FS_UI64, taskid);

	if (0 != locked_triggerids.values_num)
		DCconfig_unlock_triggers(&locked_triggerids);

	zbx_vector_uint64_destroy(&locked_triggerids);
	zbx_vector_uint64_destroy(&triggerids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s():%s", __function_name, zbx_result_string(ret));

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: tm_process_tasks                                                 *
 *                                                                            *
 * Purpose: process task manager tasks depending on task type                 *
 *                                                                            *
 * Return value: The number of successfully processed tasks                   *
 *                                                                            *
 ******************************************************************************/
static int	tm_process_tasks()
{
	DB_ROW		row;
	DB_RESULT	result;
	int		type, ret, processed_num = 0;
	zbx_uint64_t	taskid;

	result = DBselect("select taskid,type from task order by taskid");

	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(taskid, row[0]);
		ZBX_STR2UCHAR(type, row[1]);

		switch (type)
		{
			case ZBX_TM_TASK_CLOSE_PROBLEM:
				ret = tm_try_task_close_problem(taskid);
				break;
			default:
				THIS_SHOULD_NEVER_HAPPEN;
				ret = FAIL;
				break;
		}

		if (FAIL != ret)
			processed_num++;
	}
	DBfree_result(result);

	return 0;
}

ZBX_THREAD_ENTRY(taskmanager_thread, args)
{
	double	sec1, sec2;
	int	tasks_num = 0, sleeptime, nextcheck;

	process_type = ((zbx_thread_args_t *)args)->process_type;
	server_num = ((zbx_thread_args_t *)args)->server_num;
	process_num = ((zbx_thread_args_t *)args)->process_num;

	zabbix_log(LOG_LEVEL_INFORMATION, "%s #%d started [%s #%d]", get_program_type_string(program_type),
			server_num, get_process_type_string(process_type), process_num);

	zbx_setproctitle("%s [connecting to the database]", get_process_type_string(process_type));
	DBconnect(ZBX_DB_CONNECT_NORMAL);

	sec1 = zbx_time();
	sec2 = sec1;

	sleeptime = ZBX_TASKMANAGER_TIMEOUT - (int)sec1 % ZBX_TASKMANAGER_TIMEOUT;

	zbx_setproctitle("%s [started, idle %d sec]", get_process_type_string(process_type), sleeptime);

	for (;;)
	{
		zbx_sleep_loop(sleeptime);

		zbx_handle_log();

		zbx_setproctitle("%s [processing tasks]", get_process_type_string(process_type));

		sec1 = zbx_time();
		tasks_num = tm_process_tasks();
		sec2 = zbx_time();

		nextcheck = (int)sec1 - (int)sec1 % ZBX_TASKMANAGER_TIMEOUT + ZBX_TASKMANAGER_TIMEOUT;

		if (0 > (sleeptime = nextcheck - (int)sec2))
			sleeptime = 0;

		zbx_setproctitle("%s [processed %d task(s) in " ZBX_FS_DBL " sec, idle %d sec]",
				get_process_type_string(process_type), tasks_num, sec2 - sec1, sleeptime);

#if !defined(_WINDOWS) && defined(HAVE_RESOLV_H)
		zbx_update_resolver_conf();	/* handle /etc/resolv.conf update */
#endif
	}
}
