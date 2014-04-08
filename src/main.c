#define DEFINE_VARIABLES
#include "proxysql.h"

//static pthread_key_t tsd_key;





static gint proxy_admin_port = 0;
static gint proxy_mysql_port = 0;
//static gchar *config_file="proxysql.cnf";
static gchar *config_file=NULL;
static gint verbose = -1;

pthread_key_t tsd_key;

int errfd=0;

static GOptionEntry entries[] =
{
  { "admin-port", 0, 0, G_OPTION_ARG_INT, &proxy_admin_port, "Administration port", NULL },
  { "mysql-port", 0, 0, G_OPTION_ARG_INT, &proxy_mysql_port, "MySQL proxy port", NULL },
  { "verbose", 'v', 0, G_OPTION_ARG_INT, &verbose, "Verbose level", NULL },
  { "debug", 'd', 0, G_OPTION_ARG_INT, &gdbg, "debug", NULL },
  { "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_file, "Configuration file", NULL },
  { NULL }
};

pthread_attr_t attr;
int conn_cnt=0;

time_t laststart;

__thread l_sfp *__thr_sfp=NULL;
__thread myConnPools __thr_myconnpool;

int listen_tcp_fd;
int listen_tcp_admin_fd;
int listen_tcp_monitor_fd;
int listen_unix_fd;

pthread_t *glo_mysql_thrarr=NULL;


static const char * proxysql_pid_file() {
	static char fn[512];
	snprintf(fn, sizeof(fn), "%s", daemon_pid_file_ident);
	return fn;
}







void *mysql_thread(void *arg) {
	int admin=0;
	mysql_thread_init();
	int rc;
	int removing_hosts=0;
	long maintenance_ends=0;
	proxy_mysql_thread_t thrLD;
	rc=pthread_setspecific(tsd_key, &thrLD);
	assert(rc==0);
	thrLD.thread_id=*(int *)arg;

	// Initialize local memory allocator
	__thr_sfp=l_mem_init();

	// Initialize local connection pool
	local_mysql_connpool_init();

	if (thrLD.thread_id>=glovars.mysql_threads) { // starting admin or monitoring thread
		proxy_debug(PROXY_DEBUG_GENERIC, 4, "Started %s Thread with thread_id = %d\n", (thrLD.thread_id==glovars.mysql_threads ? "Admin" : "Monitoring") , thrLD.thread_id);
		admin=thrLD.thread_id+1-glovars.mysql_threads;
	} else {	// starting normal mysql thread
		proxy_debug(PROXY_DEBUG_GENERIC, 4, "Started MySQL Thread with thread_id = %d\n", thrLD.thread_id);
	}

	// Initialize local array of sessions
	thrLD.sessions=l_ptr_array_new();
//	thr.QC_rules=NULL;
//	thr.QCRver=0;
//	if (admin==0) { // no need for QC rules in
//		reset_QC_rules(thr.QC_rules);
//	}

	int i, nfds, r, max_fds;
	mysql_session_t *sess=NULL;

	struct pollfd *fds;
	max_fds = MIN_FDS_PER_THREAD;

	// preallocate an array of fds
	fds=(void *)g_malloc0(sizeof(struct pollfd)*MIN_FDS_PER_THREAD);


	// listen on various ports, different depending from admin status
	switch (admin) {
	case 0:	// normal mysql thread
		fds[0].fd=listen_tcp_fd;
		fds[1].fd=listen_unix_fd; // Unix Domain Socket
		fds[2].fd=proxyipc.fdIn[thrLD.thread_id]; // IPC pipe
		break;
	case 1:	// admin thread
		fds[0].fd=listen_tcp_admin_fd;
		break;
	case 2: // monitoring thread
		fds[0].fd=listen_tcp_monitor_fd;
		break;
	}
	while(glovars.shutdown==0) {
		// always wait for new connections
		fds[0].events=POLLIN;
		fds[0].revents=0;
		if (admin==0) {	// normal mysql thread
			// Unix Domain Socket
			fds[1].events=POLLIN;
			fds[1].revents=0;
			// IPC pipe
			fds[2].events=POLLIN;
			fds[2].revents=0;
			nfds=3;
		} else { nfds=1;}	// admin and monitoring thread


		// cycle through all healthy sessions
		// if the session has an active backend, prepare that fd
		for (i=0; i < thrLD.sessions->len; i++) {
			sess=l_ptr_array_index(thrLD.sessions, i);
			if (sess->healthy==1) {	
				if (sess->admin==0 && sess->server_mybe && sess->server_mybe->server_myds && sess->server_mybe->server_mycpe) {
					sess->fds[1].fd=sess->server_mybe->server_myds->fd;
					sess->last_server_poll_fd=sess->server_mybe->server_myds->fd;	
					sess->nfds=2;
				} else {
					sess->nfds=1;
				}	
				sess->status=CONNECTION_READING_CLIENT|CONNECTION_WRITING_CLIENT|CONNECTION_READING_SERVER|CONNECTION_WRITING_SERVER;
				sess->conn_poll(sess);
				int j;
				// copy pollfd from the session into the thread
				for (j=0; j<sess->nfds; j++) {
					if (sess->fds[j].events) {
						sess->fds[j].revents=0;
						//memcpy(&fds[nfds],&sess->fds[j],sizeof(struct pollfd));
						MEM_COPY_FWD(&fds[nfds],&sess->fds[j],sizeof(struct pollfd));
						nfds++;
					}
				}
			}
		}

		// poll() for all the fds of all the sessions
		r=poll(fds,nfds,glovars.mysql_poll_timeout);
		if (r == -1 && errno == EINTR)
	        continue;		
	    if (r == -1) {
	        PANIC("poll()");
		}


		if (admin==0 && fds[2].revents==POLLIN) { // admin thread is calling
			char c;
			int r;
			r=read(fds[2].fd,&c,sizeof(char));
			assert(r>=1);
			proxy_debug(PROXY_DEBUG_IPC, 4, "Got byte on thr %d from FD %d\n", thrLD.thread_id, fds[2].fd);
			gchar *admincmd=g_async_queue_pop(proxyipc.queue[thrLD.thread_id]);
			proxy_debug(PROXY_DEBUG_IPC, 4, "Got command %s on thr %d\n", admincmd, thrLD.thread_id);
			if (strncmp(admincmd,"REMOVE SERVER",20)==0) {
				
				removing_hosts=1;
				maintenance_ends=monotonic_time()+glovars.mysql_maintenance_timeout*1000;
			}
			g_free(admincmd);
		}


		if (admin==0 && removing_hosts==1) { // admin thread is forcing the mysql thread to remove a server
			int i;
			int cnt=0;
			for (i=0; i < thrLD.sessions->len; i++) {
				sess=l_ptr_array_index(thrLD.sessions, i);
				cnt+=sess->remove_all_backends_offline_soft(sess);
			}
			if (cnt==0) {
				removing_hosts=0;
				gchar *ack=g_malloc0(20);
				sprintf(ack,"%d",cnt);
				proxy_debug(PROXY_DEBUG_IPC, 4, "Sending ACK from thr %d\n", thrLD.thread_id);
				g_async_queue_push(proxyipc.queue[glovars.mysql_threads],ack);
			} else {
				long ct=monotonic_time();
				if (ct > maintenance_ends) {
					// drop all connections that aren't switched yet
					int i;
					int t=0;
					for (i=0; i < thrLD.sessions->len; i++) {
						int c=0;
						sess=l_ptr_array_index(thrLD.sessions, i);
						c=sess->remove_all_backends_offline_soft(sess);
						if (c) {
							t+=c;
							sess->force_close_backends=1;
							sess->close(sess);
						}
					}
					removing_hosts=0;
					gchar *ack=g_malloc0(20);
					sprintf(ack,"%d",t);
					proxy_debug(PROXY_DEBUG_IPC, 4, "Sending ACK from thr %d\n", thrLD.thread_id);
					g_async_queue_push(proxyipc.queue[glovars.mysql_threads],ack);
				}
			}
		}


		if (admin==0) {nfds=3;} else {nfds=1;} // define the starting point for fds array


		// cycle through all healthy sessions
		// and copy the fds back the the sessions
		for (i=0; i < thrLD.sessions->len; i++) {
			// copy pollfd back from the thread into the session
			sess=l_ptr_array_index(thrLD.sessions, i);
			if (sess->healthy==1) {	
				int j;
				for (j=0; j<sess->nfds; j++) {
					if (sess->fds[j].events) {
						//memcpy(&sess->fds[j],&fds[nfds],sizeof(struct pollfd));
						MEM_COPY_FWD(&sess->fds[j],&fds[nfds],sizeof(struct pollfd));
						nfds++;
					}
				}
				// handle the session. This is the real core of the proxy
				sess->check_fds_errors(sess);
				sess->handler(sess);
			}
		}

		// cycle through all sessions
		// remove and delete all unhealthy session
		for (i=0; i < thrLD.sessions->len; i++) {
			sess=l_ptr_array_index(thrLD.sessions, i);
			if (sess->healthy==0) {
				l_ptr_array_remove_index_fast(thrLD.sessions,i);
				i--;
				mysql_session_delete(sess);
			}
		}


		// This section manages new incoming connections via TCP
		if (fds[0].revents==POLLIN) {
			int c=0;
			switch (admin) {
			case 0:	// mysql thread
				c=accept(listen_tcp_fd, NULL, NULL);
				break;
			case 1:	// admin thread
				c=accept(listen_tcp_admin_fd, NULL, NULL);
				break;
			case 2:	// monitoring thread
				c=accept(listen_tcp_monitor_fd, NULL, NULL);
				break;
			}
			if (c>0) {	// this thread got the new connection
				int arg_on=1;
				setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (char *) &arg_on, sizeof(int));
				mysql_session_t *ses=mysql_session_new(&thrLD, c);
				ses->admin=admin;	// make the session aware of what sort of session is
				send_auth_pkt(ses);
				l_ptr_array_add(thrLD.sessions,ses);
			}
		}
		if (admin==0 && fds[1].revents==POLLIN) {
			int c=accept(listen_unix_fd, NULL, NULL);
			if (c>0) {
				mysql_session_t *ses=mysql_session_new(&thrLD, c);
				send_auth_pkt(ses);			
				l_ptr_array_add(thrLD.sessions,ses);
			}
		}
		if ( ( (thrLD.sessions->len + 10) * MAX_FDS_PER_SESSION ) > max_fds ) {
			// allocate more fds
			max_fds+=MIN_FDS_PER_THREAD;
			struct pollfd *fds_tmp=(void *)g_malloc0(sizeof(struct pollfd)*max_fds);
			memcpy(fds_tmp,fds,sizeof(struct pollfd)*nfds);
			g_free(fds);
			fds=fds_tmp;
		}
	}
	return NULL;
}




int main(int argc, char **argv) {
	gdbg=0;
	pid_t pid;
	int i, rc;

#ifdef DEBUG
	glo_debug=g_slice_alloc(sizeof(glo_debug_t));
	glo_debug->glock=0;
	glo_debug->msg_count=0;
	glo_debug->async_queue=g_async_queue_new();
	glo_debug->sfp=l_mem_init();
#endif

	g_thread_init(NULL);

	mtrace();
	rc=pthread_key_create(&tsd_key, NULL);
	assert(rc==0);
	// parse all the arguments and the config file
	main_opts(entries, &argc, &argv, &config_file);

	daemon_pid_file_ident=glovars.proxy_pidfile;
	daemon_log_ident=daemon_ident_from_argv0(argv[0]);

	rc=chdir(glovars.proxy_datadir);
	if (rc) {
		daemon_log(LOG_ERR, "Could not chdir into datadir: %s . Error: %s", glovars.proxy_datadir, strerror(errno));
		return EXIT_FAILURE;
	}


	daemon_pid_file_proc=proxysql_pid_file;
	
	pid=daemon_pid_file_is_running();
	if (pid>=0) {
		daemon_log(LOG_ERR, "Daemon already running on PID file %u", pid);
		return EXIT_FAILURE;
	}
	if (daemon_retval_init() < 0) {
		daemon_log(LOG_ERR, "Failed to create pipe.");
		return EXIT_FAILURE;
	}


/* Do the fork */
	if ((pid = daemon_fork()) < 0) {
		/* Exit on error */
		daemon_retval_done();
		return EXIT_FAILURE;

	} else if (pid) { /* The parent */
		int ret;
		/* Wait for 20 seconds for the return value passed from the daemon process */
		if ((ret = daemon_retval_wait(20)) < 0) {
			daemon_log(LOG_ERR, "Could not recieve return value from daemon process: %s", strerror(errno));
			return EXIT_FAILURE;
		}

		if (ret) {
			daemon_log(LOG_ERR, "Daemon returned %i as return value.", ret);
		}
		return ret;
	} else { /* The daemon */

		/* Close FDs */
		if (daemon_close_all(-1) < 0) {
			daemon_log(LOG_ERR, "Failed to close all file descriptors: %s", strerror(errno));

			/* Send the error condition to the parent process */
			daemon_retval_send(1);
			goto finish;
		}

		rc=chdir(glovars.proxy_datadir);
		if (rc) {
			daemon_log(LOG_ERR, "Could not chdir into datadir: %s . Error: %s", glovars.proxy_datadir, strerror(errno));
			return EXIT_FAILURE;
		}
		/* Create the PID file */
		if (daemon_pid_file_create() < 0) {
			daemon_log(LOG_ERR, "Could not create PID file (%s).", strerror(errno));
			daemon_retval_send(2);
			goto finish;
		}



		/* Send OK to parent process */
		daemon_retval_send(0);
		errfd=open(glovars.proxy_errorlog, O_WRONLY | O_APPEND | O_CREAT , S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
		assert(errfd>0);
		dup2(errfd, STDERR_FILENO);
		close(errfd);

		proxy_error("Starting ProxySQL\n");
		daemon_log(LOG_INFO, "Sucessfully started");
		
	}

	laststart=0;
gotofork:
	if (laststart) {
		proxy_error("Angel process is waiting %d seconds before starting a new ProxySQL process\n", glovars.proxy_restart_delay);
		sleep(glovars.proxy_restart_delay);
	}
	laststart=time(NULL);
	pid = fork();
	if (pid < 0) {
		proxy_error("[FATAL]: Error in fork()\n");
		return EXIT_FAILURE;
	}
	
	if (pid) {
		int status;
		proxy_error("Angel process started ProxySQL process %d\n", pid);
		rc=waitpid(pid, &status, 0);
		if (rc==-1) {
			perror("waitpid");
			//proxy_error("[FATAL]: waitpid: %s\n", perror("waitpid"));
			return EXIT_FAILURE;
		}
		rc=WIFEXITED(status);
		if (rc) { // client exit()ed
			rc=WEXITSTATUS(status);
			if (rc==0) {
				proxy_error("Shutdown angel process\n");
				if (glovars.http_start) system("pkill -f proxysqlHTTPd");
				return 0;
				} else {
					proxy_error("ProxySQL exited with code %d . Restarting!\n", rc);
					goto gotofork;
				}
		} else {
			proxy_error("ProxySQL crashed. Restarting!\n");
			goto gotofork;
		}
	}

	if (glovars.http_start) {
		pid = fork();
		if (!pid) {
			system("pkill -f proxysqlHTTPd");
			sleep(1);
			//execlp("perl", "perl", "-f", "./proxysqlHTTPd",NULL);
			char *execbin="./proxysqlHTTPd";
			char *newargv[] = { NULL, NULL, NULL };
			char *newenviron[] = { NULL };
			newargv[0]=execbin;
			newargv[1]=glovars.proxy_configfile;
			int rc;
			rc=chdir(glovars.proxy_datadir);
			if (rc) {
				daemon_log(LOG_ERR, "Could not chdir into datadir: %s . Error: %s", glovars.proxy_datadir, strerror(errno));
				return EXIT_FAILURE;
			}
			execve(execbin,newargv,newenviron);
			//execve(execbin,NULL,NULL);
		}
	}


	glo_DefHG_init(&gloDefHG);

	admin_init_sqlite3();

	if (glovars.merge_configfile_db==1) {
		sqlite3_flush_users_mem_to_db(sqlite3admindb,0,1);
		sqlite3_flush_debug_levels_mem_to_db(sqlite3admindb,0);
	}
	// copying back and forth should merge the data
	sqlite3_flush_debug_levels_db_to_mem(sqlite3admindb);
	sqlite3_flush_users_db_to_mem(sqlite3admindb);
	sqlite3_flush_query_rules_db_to_mem(sqlite3admindb);
	sqlite3_flush_default_hostgroups_db_to_mem(sqlite3admindb);

	sqlite3_flush_servers_mem_to_db(sqlite3admindb,0);
	sqlite3_flush_servers_db_to_mem(sqlite3admindb,1);


	//  command line options take precedences over config file
	if (proxy_admin_port) { glovars.proxy_admin_port=proxy_admin_port; }
	if (proxy_mysql_port) { glovars.proxy_mysql_port=proxy_mysql_port; }
	if (verbose>=0) { glovars.verbose=verbose; }

	if (glovars.proxy_admin_port==glovars.proxy_mysql_port) {
		proxy_error("Fatal error: proxy_admin_port (%d) matches proxy_mysql_port (%d) . Configure them to use different ports\n", glovars.proxy_admin_port, glovars.proxy_mysql_port);
		exit(EXIT_FAILURE);
	}

	if (glovars.verbose>0) {
		proxy_debug(PROXY_DEBUG_GENERIC, 1, "mysql port %d, admin port %d, config file %s, verbose %d\n", glovars.proxy_mysql_port, glovars.proxy_admin_port, config_file, verbose);
		proxy_debug(PROXY_DEBUG_QUERY_CACHE, 1, "Query cache partitions: %d\n", glovars.mysql_query_cache_partitions);
		proxy_debug(PROXY_DEBUG_MYSQL_CONNECTION, 1, "MySQL USAGE user: %s, password: _OMITTED_\n", glovars.mysql_usage_user);
		proxy_debug(PROXY_DEBUG_MYSQL_COM, 1, "Max query size: %d, Max resultset size: %d\n", glovars.mysql_max_query_size, glovars.mysql_max_resultset_size);
		//fprintf(stderr, "verbose level: %d, print_statistics_interval: %d\n", glovars.verbose, glovars.print_statistics_interval);
	}

	//glomybepools_init();


	proxy_error("Opening Sockets\n");
	listen_tcp_fd=listen_on_port((uint16_t)glovars.proxy_mysql_port);
	listen_tcp_admin_fd=listen_on_port((uint16_t)glovars.proxy_admin_port);
	listen_tcp_monitor_fd=listen_on_port((uint16_t)glovars.proxy_monitor_port);
	listen_unix_fd=listen_on_unix(glovars.mysql_socket);
	ioctl_FIONBIO(listen_tcp_fd, 1);
	ioctl_FIONBIO(listen_tcp_admin_fd, 1);
	ioctl_FIONBIO(listen_unix_fd, 1);
	//mysql_library_init(0, NULL, NULL);
	//pthread_init();
	my_init();
	mysql_server_init(0, NULL, NULL);



	// Set threads attributes . For now only setstacksize is defined
	rc=pthread_attr_init(&attr);
	rc=pthread_attr_setstacksize(&attr, glovars.stack_size);
	assert(rc==0);
	//set_thread_attr(&attr,glovars.stack_size);
	{
	size_t ss;
	rc=pthread_attr_getstacksize(&attr,&ss);
//	fprintf(stderr,"stack size=%d (%d)\n", ss, glovars.stack_size);
	}	


//	start background threads:
//	- mysql QC purger ( purgeHash_thread )
//	- mysql connection pool purger ( mysql_connpool_purge_thread )
	start_background_threads(&attr);


	init_proxyipc();

	// Note: glovars.mysql_threads+1 threads are created. The +2 is for the admin and monitoring module 
	glo_mysql_thrarr=g_malloc0(sizeof(pthread_t)*(glovars.mysql_threads+2));
	int *args=g_malloc0(sizeof(int)*(glovars.mysql_threads+2));

	// while all other threads are detachable, the mysql connections handlers are not
//	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	for (i=0; i< glovars.mysql_threads+2; i++) {
		args[i]=i;
		int rc;
		void *sp;
		rc=posix_memalign(&sp, sysconf(_SC_PAGESIZE), glovars.stack_size);
		assert(rc==0);
		rc = pthread_attr_setstack(&attr, sp, glovars.stack_size);
		assert(rc==0);
		rc=pthread_create(&glo_mysql_thrarr[i], &attr, mysql_thread , &args[i]);
		assert(rc==0);
	}


	// wait for graceful shutdown
	for (i=0; i<glovars.mysql_threads+2; i++) {
		pthread_join(glo_mysql_thrarr[i], NULL);
	}
	g_free(glo_mysql_thrarr);
	g_free(args);
	pthread_join(thread_cppt, NULL);
	pthread_join(thread_qct, NULL);
	pthread_join(thread_qr, NULL);
finish:
	daemon_log(LOG_INFO, "Exiting...");
	daemon_retval_send(255);
	daemon_signal_done();
	daemon_pid_file_remove();


	return 0;
}
