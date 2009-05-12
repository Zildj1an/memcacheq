/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *  MemcacheQ - Simple Queue Service over Memcache
 *
 *      http://memcacheq.googlecode.com
 *
 *  The source code of MemcacheQ is most based on MemcachDB:
 *
 *      http://memcachedb.googlecode.com
 *
 *  Copyright 2008 Steve Chu.  All rights reserved.
 *
 *  Use and distribution licensed under the BSD license.  See
 *  the LICENSE file for full text.
 *
 *  Authors:
 *      Steve Chu <stvchu@gmail.com>
 *
 */

#include "memcacheq.h"
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include <db.h>

static int open_exsited_queue_db(DB_TXN *txn, char *queue_name, DB **queue_dbp);
static int create_queue_db(DB_TXN *txn, char *queue_name, size_t queue_name_size, DB **queue_dbp);
static int get_queue_db_handle(DB_TXN *txn, char *queue_name, size_t queue_name_size, queue_rec_t* queue_recp);
static int update_queue_length(DB_TXN *txn, char *queue_name, size_t queue_name_size, int64_t delta);
static void close_queue_db_list(void);

static void *bdb_chkpoint_thread __P((void *));
static void *bdb_memp_trickle_thread __P((void *));
static void *bdb_dl_detect_thread __P((void *));
static void bdb_event_callback __P((DB_ENV *, u_int32_t, void *));
static void bdb_err_callback(const DB_ENV *dbenv, const char *errpfx, const char *msg);
static void bdb_msg_callback(const DB_ENV *dbenv, const char *msg);

static pthread_t chk_ptid;
static pthread_t mtri_ptid;
static pthread_t dld_ptid;

void bdb_settings_init(void)
{
    bdb_settings.env_home = DBHOME;
    bdb_settings.cache_size = 64 * 1024 * 1024; /* default is 64MB */ 
    bdb_settings.txn_lg_bsize = 32 * 1024; /* default is 32KB */ 
    
    /* queue only */
    bdb_settings.re_len = 1024;
    bdb_settings.q_extentsize = 131072;
    
    bdb_settings.page_size = 4096;  /* default is 4K */
    bdb_settings.txn_nosync = 0; /* default DB_TXN_NOSYNC is off */
    bdb_settings.dldetect_val = 100 * 1000; /* default is 100 millisecond */
    bdb_settings.chkpoint_val = 60 * 5;
    bdb_settings.memp_trickle_val = 30;
    bdb_settings.memp_trickle_percent = 60; 
    bdb_settings.db_flags = DB_CREATE | DB_AUTO_COMMIT;
    bdb_settings.env_flags = DB_CREATE
                          | DB_INIT_LOCK 
                          | DB_THREAD 
                          | DB_INIT_MPOOL 
                          | DB_INIT_LOG 
                          | DB_INIT_TXN
                          | DB_RECOVER;
                              
}

void bdb_env_init(void){
    int ret;
    /* db env init */
    if ((ret = db_env_create(&envp, 0)) != 0) {
        fprintf(stderr, "db_env_create: %s\n", db_strerror(ret));
        exit(EXIT_FAILURE);
    }

    /* set err&msg display */
    envp->set_errpfx(envp, PACKAGE);
    /* env->set_errfile(env, stderr); */
    /* env->set_msgfile(env, stderr); */
	envp->set_errcall(envp, bdb_err_callback);
  	envp->set_msgcall(envp, bdb_msg_callback);
  	
    /* set BerkeleyDB verbose*/
    if (settings.verbose > 1) {
        if ((ret = envp->set_verbose(envp, DB_VERB_FILEOPS_ALL, 1)) != 0) {
            fprintf(stderr, "envp->set_verbose[DB_VERB_FILEOPS_ALL]: %s\n",
                    db_strerror(ret));
            exit(EXIT_FAILURE);
        }
        if ((ret = envp->set_verbose(envp, DB_VERB_DEADLOCK, 1)) != 0) {
            fprintf(stderr, "envp->set_verbose[DB_VERB_DEADLOCK]: %s\n",
                    db_strerror(ret));
            exit(EXIT_FAILURE);
        }
        if ((ret = envp->set_verbose(envp, DB_VERB_RECOVERY, 1)) != 0) {
            fprintf(stderr, "envp->set_verbose[DB_VERB_RECOVERY]: %s\n",
                    db_strerror(ret));
            exit(EXIT_FAILURE);
        }
    }

    /* set MPOOL size */
    envp->set_cachesize(envp, 0, bdb_settings.cache_size, 0);

    /* set DB_TXN_NOSYNC flag */
    if (bdb_settings.txn_nosync){
        envp->set_flags(envp, DB_TXN_NOSYNC, 1);
    }

    /* set locking */
    envp->set_lk_max_lockers(envp, 20000);
    envp->set_lk_max_locks(envp, 20000);
    envp->set_lk_max_objects(envp, 20000);
    envp->set_tx_max(envp, 20000);
    
    /* at least max active transactions */
  	envp->set_tx_max(envp, 10000);
  	
    /* set transaction log buffer */
    envp->set_lg_bsize(envp, bdb_settings.txn_lg_bsize);
    
    /* if no home dir existed, we create it */
    if (0 != access(bdb_settings.env_home, F_OK)) {
        if (0 != mkdir(bdb_settings.env_home, 0750)) {
            fprintf(stderr, "mkdir env_home error:[%s]\n", bdb_settings.env_home);
            exit(EXIT_FAILURE);
        }
    }
    
    if ((ret = envp->open(envp, bdb_settings.env_home, bdb_settings.env_flags, 0)) != 0) {
        fprintf(stderr, "db_env_open: %s\n", db_strerror(ret));
        exit(EXIT_FAILURE);
    }

}


void bdb_qlist_db_open(void){
    int ret;
    int db_open = 0;
    DBC *cursorp = NULL;
    DB_TXN *txn = NULL;
    DBT dbkey, dbdata;
    char queue_name[512];
    queue_rec_t queue_rec;
    DB *queue_dbp = NULL;
    
    u_int32_t qlist_db_flags = DB_CREATE;
    
    ret = envp->txn_begin(envp, NULL, &txn, 0);
    if (ret != 0) {
        goto err;
    }
    
    /* for replicas to get a full master copy, then open db */
    while(!db_open) {
        /* close the queue list db */
        if (qlist_dbp != NULL) {
            qlist_dbp->close(qlist_dbp, 0);
            qlist_dbp = NULL;
        }

        if ((ret = db_create(&qlist_dbp, envp, 0)) != 0) {
            fprintf(stderr, "db_create: %s\n", db_strerror(ret));
            exit(EXIT_FAILURE);
        }
        
        if ((ret = qlist_dbp->set_priority(qlist_dbp, DB_PRIORITY_VERY_HIGH)) != 0){
            fprintf(stderr, "qlist_dbp->set_priority: %s\n", db_strerror(ret));
            exit(EXIT_FAILURE);
        }
        
        /*
        if ((ret = qlist_dbp->set_pagesize(qlist_dbp, 512)) != 0){
            fprintf(stderr, "qlist_dbp->set_pagesize: %s\n", db_strerror(ret));
            exit(EXIT_FAILURE);
        }
        */

        /* try to open qlist db*/
        ret = qlist_dbp->open(qlist_dbp, txn, "queue.list", NULL, DB_BTREE, qlist_db_flags, 0664);         
        switch (ret){
        case 0:
            db_open = 1;
            break;
        case ENOENT:
        case DB_LOCK_DEADLOCK:
            fprintf(stderr, "bdb_qlist_db_open: %s\n", db_strerror(ret));
            sleep(3);
            break;
        default:
            fprintf(stderr, "bdb_qlist_db_open: %s\n", db_strerror(ret));
            goto err;
        }
    }
    
    /* Get a cursor */
    ret = qlist_dbp->cursor(qlist_dbp, txn, &cursorp, 0); 
    if (ret != 0) {
        goto err;
    }
    
    /* Initialize our DBTs. */
    BDB_CLEANUP_DBT();
    memset(queue_name, 0, 512);
    
    dbkey.data = (void *)queue_name;
    dbkey.ulen = 512;
    dbkey.flags = DB_DBT_USERMEM;
    dbdata.data = (void *)&queue_rec;
    dbdata.ulen = sizeof(queue_rec);
    dbdata.flags = DB_DBT_USERMEM;
    
    /* Iterate over the database, retrieving each record in turn. */
    while ((ret = cursorp->get(cursorp, &dbkey, &dbdata, DB_NEXT)) == 0) {
        memset(&queue_rec, 0, sizeof(queue_rec_t));
        ret = open_exsited_queue_db(txn, queue_name, &queue_rec.queue_dbp);
        if (ret != 0){
            goto err;
        }
        ret = cursorp->put(cursorp, &dbkey, &dbdata, DB_CURRENT);
        if (ret != 0){
            goto err;
        }
    }
    if (ret != DB_NOTFOUND) {
        goto err;
    }
    
    if (cursorp != NULL){
        cursorp->close(cursorp);
    }
    
    ret = txn->commit(txn, 0);
    if (ret != 0) {
        goto err;
    }
    
    return;
        
err:
    if (cursorp != NULL){
        cursorp->close(cursorp);
    }
    if (txn != NULL){
        txn->abort(txn);
    }
    fprintf(stderr, "bdb_qlist_db_open: %s %s\n", queue_name, db_strerror(ret));
    exit(EXIT_FAILURE);

}

static int open_exsited_queue_db(DB_TXN *txn, char *queue_name, DB **queue_dbp){
    int ret, db_open;
    u_int32_t db_flags = DB_CREATE;
    DB *temp_dbp = NULL;
    db_open = 0;
    
    /* for replicas to get a full master copy, then open db */
    while(!db_open) {
        if (temp_dbp != NULL){
            temp_dbp->close(temp_dbp, 0);
            temp_dbp = NULL;
        }

        if ((ret = db_create(&temp_dbp, envp, 0)) != 0) {
            fprintf(stderr, "db_create: %s\n", db_strerror(ret));
            goto err;
        }
        
        /* set record length */
        if (bdb_settings.q_extentsize != 0){
            if((ret = temp_dbp->set_q_extentsize(temp_dbp, bdb_settings.q_extentsize)) != 0){
                fprintf(stderr, "temp_dbp[%s]->set_q_extentsize: %s\n", queue_name, db_strerror(ret));
                goto err;
            }
        }
        
        /* set record length */
        if((ret = temp_dbp->set_re_len(temp_dbp, bdb_settings.re_len)) != 0){
            fprintf(stderr, "temp_dbp[%s]->set_re_len: %s\n", queue_name, db_strerror(ret));
            goto err;
        }
        
        /* set page size */
        if((ret = temp_dbp->set_pagesize(temp_dbp, bdb_settings.page_size)) != 0){
            fprintf(stderr, "temp_dbp[%s]->set_pagesize: %s\n", queue_name, db_strerror(ret));
            goto err;
        }
    
        /* try to open db*/
        ret = temp_dbp->open(temp_dbp, txn, queue_name, NULL, DB_QUEUE, db_flags, 0664);         
        switch (ret){
        case 0:
            db_open = 1;
            *queue_dbp = temp_dbp;
            break;
        case ENOENT:
        case DB_LOCK_DEADLOCK:
            fprintf(stderr, "temp_dbp[%s]->open: %s\n", queue_name, db_strerror(ret));
            sleep(2);
            break;
        default:
            goto err;
        }
    }
    return 0;
    
err:
    if (temp_dbp != NULL){
        temp_dbp->close(temp_dbp, 0);
    }
    return ret;
}

static int create_queue_db(DB_TXN *txn, char *queue_name, size_t queue_name_size, DB **queue_dbp){
    int ret;
    u_int32_t db_flags = DB_CREATE;
    queue_rec_t queue_rec;
    DB *temp_dbp = NULL;
    DBT dbkey,dbdata;
    
    /* DB handle */
    if ((ret = db_create(&temp_dbp, envp, 0)) != 0) {
        goto err;
    }

    /* configure */
    if (bdb_settings.q_extentsize != 0){
        if((ret = temp_dbp->set_q_extentsize(temp_dbp, bdb_settings.q_extentsize)) != 0){
            goto err;
        }
    }
    if((ret = temp_dbp->set_re_len(temp_dbp, bdb_settings.re_len)) != 0){
        goto err;
    }
    if((ret = temp_dbp->set_pagesize(temp_dbp, bdb_settings.page_size)) != 0){
        goto err;
    }
    
    /* try to open db*/
    ret = temp_dbp->open(temp_dbp, txn, queue_name, NULL, DB_QUEUE, db_flags, 0664); 
    if (ret != 0){
        goto err;
    }

    queue_rec.queue_dbp = temp_dbp;
    queue_rec.queue_size = 0;

    BDB_CLEANUP_DBT();
    dbkey.data = (void *)queue_name;
    dbkey.size = queue_name_size;
    dbdata.data = (void *)&queue_rec;
    dbdata.size = sizeof(queue_rec_t);
    
    ret = qlist_dbp->put(qlist_dbp, txn, &dbkey, &dbdata, 0);
    if (ret != 0){
        goto err;
    }
    
    *queue_dbp = temp_dbp;
    return 0;
    
err:
    if (temp_dbp != NULL){
        temp_dbp->close(temp_dbp, 0);
    }
    return ret;
}

int delete_queue_db(char *queue_name, size_t queue_name_size){
    DBT dbkey, dbdata;
    int ret;
    DB_TXN *txn = NULL;
    queue_rec_t queue_rec;
    DB *queue_dbp = NULL;
    
    BDB_CLEANUP_DBT();
    dbkey.data = (void *)queue_name;
    dbkey.size = queue_name_size;
    
    ret = envp->txn_begin(envp, NULL, &txn, 0);
    if (ret != 0) {
        goto err;
    }

    memset(&queue_rec, 0, sizeof(queue_rec_t));
    ret = get_queue_db_handle(txn, queue_name, queue_name_size, &queue_rec);
    queue_dbp = queue_rec.queue_dbp;

    if (ret != 0 || queue_dbp == NULL){
        goto err;
    }
    
    ret = queue_dbp->close(queue_dbp, 0);
    if (ret != 0 ){
        goto err;
    }
    
    ret = envp->dbremove(envp, txn, queue_name, NULL, 0);
    if (ret != 0){
        goto err;
    }
    
    ret = qlist_dbp->del(qlist_dbp, txn, &dbkey, 0);
    if (ret != 0){
        goto err;
    }
    
    ret = txn->commit(txn, 0);
    if (ret != 0) {
        goto err;
    }
    return 0;

err:
    if (txn != NULL){
        txn->abort(txn);
    }
    if (settings.verbose > 1) {
        fprintf(stderr, "delete_queue_db: %s\n", db_strerror(ret));
    }
    return 1;
}

static int get_queue_db_handle(DB_TXN *txn, char *queue_name, size_t queue_name_size, queue_rec_t *queue_recp){
    DBT dbkey, dbdata;
    int ret;
    DB *temp_dbp = NULL;
    
    memset(queue_recp, 0, sizeof(queue_rec_t));

    BDB_CLEANUP_DBT();
    dbkey.data = (void *)queue_name;
    dbkey.size = queue_name_size;
    dbdata.data = (void *)queue_recp;
    dbdata.ulen = sizeof(queue_rec_t);
    dbdata.flags = DB_DBT_USERMEM;
    
    ret = qlist_dbp->get(qlist_dbp, txn, &dbkey, &dbdata, 0);
    if (ret == 0){
    } else if (ret == DB_NOTFOUND){
        queue_recp->queue_dbp = NULL;
    } else {
        return ret;
    }
    
    return 0;
}

static int update_queue_length(DB_TXN *txn, char *queue_name, size_t queue_name_size, int64_t delta)
{
    DBT dbkey, dbdata;
    int ret;
    queue_rec_t queue_rec;

    BDB_CLEANUP_DBT();
    dbkey.data = (void *)queue_name;
    dbkey.size = queue_name_size;
    dbdata.data = (void *)&queue_rec;
    dbdata.ulen = sizeof(queue_rec_t);
    dbdata.flags = DB_DBT_USERMEM;

    
    ret = qlist_dbp->get(qlist_dbp, txn, &dbkey, &dbdata, 0);
    if (ret == 0){
        queue_rec.queue_size += delta;
        ret = qlist_dbp->put(qlist_dbp, txn, &dbkey, &dbdata, 0);
    }

    return ret;
}


int print_queue_db_list(char *buf, size_t buf_size){
    DBT dbkey, dbdata;
    int ret, res;
    DB_TXN *txn = NULL;
    DBC *cursorp = NULL;
    char queue_name[512];
    queue_rec_t queue_rec;
    DB *queue_db = NULL;
    int remains = buf_size - 5;
    
    memset(queue_name, 0, 512);
    BDB_CLEANUP_DBT();
    dbkey.data = (void *)queue_name;
    dbkey.ulen = 512;
    dbkey.flags = DB_DBT_USERMEM;
    dbdata.data = (void *)&queue_rec;
    dbdata.ulen = sizeof(queue_rec_t);
    dbdata.flags = DB_DBT_USERMEM;

    ret = envp->txn_begin(envp, NULL, &txn, 0);
    if (ret != 0) {
        goto err;
    }
    
    /* Get a cursor */
    ret = qlist_dbp->cursor(qlist_dbp, txn, &cursorp, 0); 
    if (ret != 0){
        goto err;
    }
    
    /* Iterate over the database, retrieving each record in turn. */
    while ((ret = cursorp->get(cursorp, &dbkey, &dbdata, DB_NEXT)) == 0) {
        queue_db = queue_rec.queue_dbp;
        queue_name[dbkey.size] = '\0';
        if (remains > strlen(queue_name) + 8){
            res = sprintf(buf, "STAT %s %d\r\n", queue_name, queue_rec.queue_size);
            remains -= res;
            buf += res; 
        } else {
            break;
        }
    }
    if (!(ret == DB_NOTFOUND || ret == 0)) {
        goto err;
    }
    
    if (cursorp != NULL){
        cursorp->close(cursorp);
    }

    ret = txn->commit(txn, 0);
    if (ret != 0) {
        goto err;
    }
    sprintf(buf, "END");
    return 0;

err:
    if (cursorp != NULL){
        cursorp->close(cursorp);
    }
    if (txn != NULL){
        txn->abort(txn);
    }
    if (settings.verbose > 1) {
        fprintf(stderr, "print_queue_db_list: %s\n", db_strerror(ret));
    }
    return -1;
}

static void close_queue_db_list(void){
    DBT dbkey, dbdata;
    int ret;
    DB_TXN *txn = NULL;
    DBC *cursorp = NULL;
    char queue_name[512];
    queue_rec_t queue_rec;
    DB *queue_dbp = NULL;

    memset(queue_name, 0, 512);
    BDB_CLEANUP_DBT();
    dbkey.data = (void *)queue_name;
    dbkey.ulen = 512;
    dbkey.flags = DB_DBT_USERMEM;
    dbdata.data = (void *)&queue_rec;
    dbdata.ulen = sizeof(queue_rec_t);
    dbdata.flags = DB_DBT_USERMEM;

    ret = envp->txn_begin(envp, NULL, &txn, 0);
    if (ret != 0) {
        goto err;
    }

    /* Get a cursor */
    ret = qlist_dbp->cursor(qlist_dbp, txn, &cursorp, 0); 
    if (ret != 0){
        goto err;
    }

    /* Iterate over the database, retrieving each record in turn. */
    while ((ret = cursorp->get(cursorp, &dbkey, &dbdata, DB_NEXT)) == 0) {
        queue_dbp = queue_rec.queue_dbp;
        ret = queue_dbp->close(queue_dbp, 0);
        if (settings.verbose > 1) {
            fprintf(stderr, "close_queue_db_list: %s %s\n", queue_name, db_strerror(ret));
        }
    }
    if (ret != DB_NOTFOUND) {
        goto err;
    }

    if (cursorp != NULL){
        cursorp->close(cursorp);
    }

    ret = txn->commit(txn, 0);
    if (ret != 0) {
        goto err;
    }
    return;

err:
    if (cursorp != NULL){
        cursorp->close(cursorp);
    }
    if (txn != NULL){
        txn->abort(txn);
    }
    if (settings.verbose > 1) {
        fprintf(stderr, "close_queue_db_list: %s\n", db_strerror(ret));
    }
    return;
}

/* if return item is not NULL, free by caller */
item *bdb_get(char *key, size_t nkey){
    item *it = NULL;
    DBT dbkey, dbdata;
    DB_TXN *txn = NULL;
    queue_rec_t queue_rec;
    DB *queue_dbp = NULL;
    db_recno_t recno;
    int ret;
    
    /* first, alloc a fixed size */
    it = item_alloc2();
    if (it == 0) {
        return NULL;
    }

    BDB_CLEANUP_DBT();
    dbkey.data = &recno;
    dbkey.ulen = sizeof(recno);
    dbkey.flags = DB_DBT_USERMEM;
    dbdata.ulen = bdb_settings.re_len;
    dbdata.data = it;
    dbdata.flags = DB_DBT_USERMEM;
    
    ret = envp->txn_begin(envp, NULL, &txn, 0);
    if (ret != 0) {
        goto err;
    }
    
    memset(&queue_rec, 0, sizeof(queue_rec_t));
    ret = get_queue_db_handle(txn, key, nkey, &queue_rec);
    queue_dbp = queue_rec.queue_dbp;

    if (ret != 0 || queue_dbp == NULL){
        goto err;
    }
    
    ret = queue_dbp->get(queue_dbp, txn, &dbkey, &dbdata, DB_CONSUME);
    if (ret != 0){
        goto err;
    }
    if (settings.max_queue_size) {
        UPDATE_QUEUE_LENGTH_LOCK();
        ret = update_queue_length(txn, key, nkey, -1);
        UPDATE_QUEUE_LENGTH_UNLOCK();
        if (ret != 0) {
            goto err;
        }
    }

    ret = txn->commit(txn, 0);
    if (ret != 0) {
        goto err;
    }
    return it;
err:
    item_free(it);
    it = NULL;
    if (txn != NULL){
        txn->abort(txn);
    }
    if (settings.verbose > 1) {
        fprintf(stderr, "bdb_get: %s\n", db_strerror(ret));
    }
    return NULL;
}

/* 0 for Success
   -1 for SERVER_ERROR
*/
int bdb_put(char *key, size_t nkey, item *it){
    int ret;
    DBT dbkey, dbdata;
    DB_TXN *txn = NULL;
    queue_rec_t queue_rec;
    DB *queue_dbp = NULL;
    unsigned int queue_size;
    db_recno_t recno;

    BDB_CLEANUP_DBT();
    dbkey.data = &recno;
    dbkey.ulen = sizeof(recno);
    dbkey.flags = DB_DBT_USERMEM;
    dbdata.data = it;
    dbdata.size = ITEM_ntotal(it);
    
    ret = envp->txn_begin(envp, NULL, &txn, 0);
    if (ret != 0) {
        goto err;
    }
    
    memset(&queue_rec, 0, sizeof(queue_rec_t));
    ret = get_queue_db_handle(txn, key, nkey, &queue_rec);
    queue_dbp = queue_rec.queue_dbp;

    if (ret != 0){
        goto err;
    }

    if (queue_dbp == NULL) {
        ret = create_queue_db(txn, key, nkey, &queue_dbp);
        if (ret != 0){
            goto err;
        }
    }

    if (settings.max_queue_size && queue_rec.queue_size + 1 > settings.max_queue_size) {
        if (txn != NULL){
            txn->abort(txn);
        }
        if (settings.verbose > 1) {
            fprintf(stderr, "bdb_put: queue size limited %d\n", settings.max_queue_size);
        }
        return -1;
    }

    ret = queue_dbp->put(queue_dbp, txn, &dbkey, &dbdata, DB_APPEND);
    if (ret != 0) {
        goto err;
    }
  
    if (settings.max_queue_size) {
        UPDATE_QUEUE_LENGTH_LOCK();
        ret = update_queue_length(txn, key, nkey, 1);
        UPDATE_QUEUE_LENGTH_UNLOCK();
        if (ret != 0) {
            goto err;
        }
    }
    
    ret = txn->commit(txn, 0);
    if (ret != 0) {
        goto err;
    }
    
    return 0;
err:
    if (txn != NULL){
        txn->abort(txn);
    }
    if (settings.verbose > 1) {
        fprintf(stderr, "bdb_put: %s\n", db_strerror(ret));
    }
    return -1;
}

void start_chkpoint_thread(void){
    if (bdb_settings.chkpoint_val > 0){
        /* Start a checkpoint thread. */
        if ((errno = pthread_create(
            &chk_ptid, NULL, bdb_chkpoint_thread, (void *)envp)) != 0) {
            fprintf(stderr,
                "failed spawning checkpoint thread: %s\n",
                strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
}

void start_memp_trickle_thread(void){
    if (bdb_settings.memp_trickle_val > 0){
        /* Start a memp_trickle thread. */
        if ((errno = pthread_create(
            &mtri_ptid, NULL, bdb_memp_trickle_thread, (void *)envp)) != 0) {
            fprintf(stderr,
                "failed spawning memp_trickle thread: %s\n",
                strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
}

void start_dl_detect_thread(void){
    if (bdb_settings.dldetect_val > 0){
        /* Start a deadlock detecting thread. */
        if ((errno = pthread_create(
            &dld_ptid, NULL, bdb_dl_detect_thread, (void *)envp)) != 0) {
            fprintf(stderr,
                "failed spawning deadlock thread: %s\n",
                strerror(errno));
            exit(EXIT_FAILURE);
        }
    }
}

static void *bdb_chkpoint_thread(void *arg)
{
    DB_ENV *dbenv;
    int ret;
    dbenv = arg;
    if (settings.verbose > 1) {
        dbenv->errx(dbenv, "checkpoint thread created: %lu, every %d seconds", 
                           (u_long)pthread_self(), bdb_settings.chkpoint_val);
    }
    for (;; sleep(bdb_settings.chkpoint_val)) {
        if ((ret = dbenv->txn_checkpoint(dbenv, 0, 0, 0)) != 0) {
            dbenv->err(dbenv, ret, "checkpoint thread");
        }
        dbenv->errx(dbenv, "checkpoint thread: a txn_checkpoint is done");
    }
    return (NULL);
}

static void *bdb_memp_trickle_thread(void *arg)
{
    DB_ENV *dbenv;
    int ret, nwrotep;
    dbenv = arg;
    if (settings.verbose > 1) {
        dbenv->errx(dbenv, "memp_trickle thread created: %lu, every %d seconds, %d%% pages should be clean.", 
                           (u_long)pthread_self(), bdb_settings.memp_trickle_val,
                           bdb_settings.memp_trickle_percent);
    }
    for (;; sleep(bdb_settings.memp_trickle_val)) {
        if ((ret = dbenv->memp_trickle(dbenv, bdb_settings.memp_trickle_percent, &nwrotep)) != 0) {
            dbenv->err(dbenv, ret, "memp_trickle thread");
        }
        dbenv->errx(dbenv, "memp_trickle thread: writing %d dirty pages", nwrotep);
    }
    return (NULL);
}

static void *bdb_dl_detect_thread(void *arg)
{
    DB_ENV *dbenv;
    struct timeval t;
    dbenv = arg;
    if (settings.verbose > 1) {
        dbenv->errx(dbenv, "deadlock detecting thread created: %lu, every %d millisecond",
                           (u_long)pthread_self(), bdb_settings.dldetect_val);
    }
    while (!daemon_quit) {
        t.tv_sec = 0;
        t.tv_usec = bdb_settings.dldetect_val;
        (void)dbenv->lock_detect(dbenv, 0, DB_LOCK_YOUNGEST, NULL);
        /* select is a more accurate sleep timer */
        (void)select(0, NULL, NULL, NULL, &t);
    }
    return (NULL);
}

static void bdb_event_callback(DB_ENV *env, u_int32_t which, void *info)
{
    switch (which) {
    case DB_EVENT_PANIC:
        env->errx(env, "evnet: DB_EVENT_PANIC, we got panic, recovery should be run.");
        break;
    case DB_EVENT_WRITE_FAILED:
        env->errx(env, "event: DB_EVENT_WRITE_FAILED, I wrote to stable storage failed.");
        break;
    default:
        env->errx(env, "ignoring event %d", which);
    }
}

static void bdb_err_callback(const DB_ENV *dbenv, const char *errpfx, const char *msg){
	time_t curr_time = time(NULL);
	char time_str[32];
	strftime(time_str, 32, "%c", localtime(&curr_time));
	fprintf(stderr, "[%s] [%s] \"%s\"\n", errpfx, time_str, msg);
}

static void bdb_msg_callback(const DB_ENV *dbenv, const char *msg){
	time_t curr_time = time(NULL);
	char time_str[32];
	strftime(time_str, 32, "%c", localtime(&curr_time));
	fprintf(stderr, "[%s] [%s] \"%s\"\n", PACKAGE, time_str, msg);
}

/* for atexit cleanup */
void bdb_chkpoint(void)
{
    int ret = 0;
    if (envp != NULL){
        ret = envp->txn_checkpoint(envp, 0, 0, 0); 
        if (0 != ret){
            fprintf(stderr, "envp->txn_checkpoint: %s\n", db_strerror(ret));
        }else{
            fprintf(stderr, "envp->txn_checkpoint: OK\n");
        }
    }
}

/* for atexit cleanup */
void bdb_db_close(void){
    int ret = 0;
    
    /* close the queue list db */
    if (qlist_dbp != NULL) {
        close_queue_db_list();
        ret = qlist_dbp->close(qlist_dbp, 0);
        if (0 != ret){
            fprintf(stderr, "qlist_dbp->close: %s\n", db_strerror(ret));
        }else{
            qlist_dbp = NULL;
            fprintf(stderr, "qlist_dbp->close: OK\n");
        }
    }
}

/* for atexit cleanup */
void bdb_env_close(void){
    int ret = 0;
    if (envp != NULL) {
        ret = envp->close(envp, 0);
        if (0 != ret){
            fprintf(stderr, "envp->close: %s\n", db_strerror(ret));
        }else{
            envp = NULL;
            fprintf(stderr, "envp->close: OK\n");
        }
    }
}
