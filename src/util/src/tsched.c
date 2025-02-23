/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "os.h"
#include "taosdef.h"
#include "tutil.h"
#include "tulog.h"
#include "tsched.h"
#include "ttimer.h"

#define DUMP_SCHEDULER_TIME_WINDOW 30000 //every 30sec, take a snap shot of task queue.

typedef struct {
  char            label[TSDB_LABEL_LEN];
  tsem_t          emptySem;
  tsem_t          fullSem;
  pthread_mutex_t queueMutex;
  int             fullSlot;
  int             emptySlot;
  int             queueSize;
  int             numOfThreads;
  pthread_t *     qthread;
  SSchedMsg *     queue;
  bool            stop;
  void*           pTmrCtrl;
  void*           pTimer;
} SSchedQueue;

static void *taosProcessSchedQueue(void *param);
static void taosDumpSchedulerStatus(void *qhandle, void *tmrId);

// 初始化调度器
void *taosInitScheduler(int queueSize, int numOfThreads, const char *label) {
  // 为这个队列结构体申请内存
  SSchedQueue *pSched = (SSchedQueue *)calloc(sizeof(SSchedQueue), 1);
  if (pSched == NULL) {
    uError("%s: no enough memory for pSched", label);
    return NULL;
  }

  // 真正的队列申请内存
  // 一个消息的长度 * 队列长度
  pSched->queue = (SSchedMsg *)calloc(sizeof(SSchedMsg), queueSize);
  if (pSched->queue == NULL) {
    uError("%s: no enough memory for queue", label);
    taosCleanUpScheduler(pSched);
    return NULL;
  }

  // 申请线程内存空间
  pSched->qthread = calloc(sizeof(pthread_t), numOfThreads);
  if (pSched->qthread == NULL) {
    uError("%s: no enough memory for qthread", label);
    taosCleanUpScheduler(pSched);
    return NULL;
  }

  pSched->queueSize = queueSize;
  // 复制字符串
  tstrncpy(pSched->label, label, sizeof(pSched->label)); // fix buffer overflow

  pSched->fullSlot = 0;
  pSched->emptySlot = 0;

  // 初始化锁
  if (pthread_mutex_init(&pSched->queueMutex, NULL) < 0) {
    uError("init %s:queueMutex failed(%s)", label, strerror(errno));
    taosCleanUpScheduler(pSched);
    return NULL;
  }

  // 初始化信号量，队列为空的信号量
  if (tsem_init(&pSched->emptySem, 0, (unsigned int)pSched->queueSize) != 0) {
    uError("init %s:empty semaphore failed(%s)", label, strerror(errno));
    taosCleanUpScheduler(pSched);
    return NULL;
  }

  // 初始化信号量， 队列满的信号量
  if (tsem_init(&pSched->fullSem, 0, 0) != 0) {
    uError("init %s:full semaphore failed(%s)", label, strerror(errno));
    taosCleanUpScheduler(pSched);
    return NULL;
  }

  // 调度器 stop设置为false
  pSched->stop = false;
  // 初始化所有线程
  for (int i = 0; i < numOfThreads; ++i) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    int code = pthread_create(pSched->qthread + i, &attr, taosProcessSchedQueue, (void *)pSched);
    pthread_attr_destroy(&attr);
    if (code != 0) {
      uError("%s: failed to create rpc thread(%s)", label, strerror(errno));
      taosCleanUpScheduler(pSched);
      return NULL;
    }
    ++pSched->numOfThreads;
  }

  uDebug("%s scheduler is initialized, numOfThreads:%d", label, pSched->numOfThreads);

  // 返回创建好的调度器
  return (void *)pSched;
}

void *taosInitSchedulerWithInfo(int queueSize, int numOfThreads, const char *label, void *tmrCtrl) {
  SSchedQueue* pSched = taosInitScheduler(queueSize, numOfThreads, label);
  
  if (tmrCtrl != NULL && pSched != NULL) {
    pSched->pTmrCtrl = tmrCtrl;
    taosTmrReset(taosDumpSchedulerStatus, DUMP_SCHEDULER_TIME_WINDOW, pSched, pSched->pTmrCtrl, &pSched->pTimer);
  }
  
  return pSched;
}

void *taosProcessSchedQueue(void *param) {
  SSchedMsg    msg;
  SSchedQueue *pSched = (SSchedQueue *)param;

  while (1) {
    if (tsem_wait(&pSched->fullSem) != 0) {
      uError("wait %s fullSem failed(%s)", pSched->label, strerror(errno));
    }
    if (pSched->stop) {
      break;
    }

    if (pthread_mutex_lock(&pSched->queueMutex) != 0)
      uError("lock %s queueMutex failed(%s)", pSched->label, strerror(errno));

    msg = pSched->queue[pSched->fullSlot];
    memset(pSched->queue + pSched->fullSlot, 0, sizeof(SSchedMsg));
    pSched->fullSlot = (pSched->fullSlot + 1) % pSched->queueSize;

    if (pthread_mutex_unlock(&pSched->queueMutex) != 0)
      uError("unlock %s queueMutex failed(%s)", pSched->label, strerror(errno));

    if (tsem_post(&pSched->emptySem) != 0)
      uError("post %s emptySem failed(%s)", pSched->label, strerror(errno));

    if (msg.fp)
      (*(msg.fp))(&msg);
    else if (msg.tfp)
      (*(msg.tfp))(msg.ahandle, msg.thandle);
  }

  return NULL;
}

int taosScheduleTask(void *qhandle, SSchedMsg *pMsg) {
  SSchedQueue *pSched = (SSchedQueue *)qhandle;
  if (pSched == NULL) {
    uError("sched is not ready, msg:%p is dropped", pMsg);
    return 0;
  }

  if (tsem_wait(&pSched->emptySem) != 0) {
    uError("wait %s emptySem failed(%s)", pSched->label, strerror(errno));
  }

  if (pthread_mutex_lock(&pSched->queueMutex) != 0)
    uError("lock %s queueMutex failed(%s)", pSched->label, strerror(errno));

  pSched->queue[pSched->emptySlot] = *pMsg;
  pSched->emptySlot = (pSched->emptySlot + 1) % pSched->queueSize;

  if (pthread_mutex_unlock(&pSched->queueMutex) != 0)
    uError("unlock %s queueMutex failed(%s)", pSched->label, strerror(errno));

  if (tsem_post(&pSched->fullSem) != 0) 
    uError("post %s fullSem failed(%s)", pSched->label, strerror(errno));

  return 0;
}

void taosCleanUpScheduler(void *param) {
  SSchedQueue *pSched = (SSchedQueue *)param;
  if (pSched == NULL) return;

  pSched->stop = true;
  for (int i = 0; i < pSched->numOfThreads; ++i) {
  if (taosCheckPthreadValid(pSched->qthread[i])) {
      tsem_post(&pSched->fullSem);
    }
  }
  for (int i = 0; i < pSched->numOfThreads; ++i) {
    if (taosCheckPthreadValid(pSched->qthread[i])) {
      pthread_join(pSched->qthread[i], NULL);
    }
  }

  tsem_destroy(&pSched->emptySem);
  tsem_destroy(&pSched->fullSem);
  pthread_mutex_destroy(&pSched->queueMutex);
  
  if (pSched->pTimer) {
    taosTmrStopA(&pSched->pTimer);
  }

  if (pSched->queue) free(pSched->queue);
  if (pSched->qthread) free(pSched->qthread);
  free(pSched); // fix memory leak
}

// for debug purpose, dump the scheduler status every 1min.
void taosDumpSchedulerStatus(void *qhandle, void *tmrId) {
  SSchedQueue *pSched = (SSchedQueue *)qhandle;
  if (pSched == NULL || pSched->pTimer == NULL || pSched->pTimer != tmrId) {
    return;
  }
  
  int32_t size = ((pSched->emptySlot - pSched->fullSlot) + pSched->queueSize) % pSched->queueSize;
  if (size > 0) {
    uDebug("scheduler:%s, current tasks in queue:%d, task thread:%d", pSched->label, size, pSched->numOfThreads);
  }
  
  taosTmrReset(taosDumpSchedulerStatus, DUMP_SCHEDULER_TIME_WINDOW, pSched, pSched->pTmrCtrl, &pSched->pTimer);
}
