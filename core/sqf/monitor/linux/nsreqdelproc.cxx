///////////////////////////////////////////////////////////////////////////////
//
// @@@ START COPYRIGHT @@@
//
// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
//
// @@@ END COPYRIGHT @@@
//
///////////////////////////////////////////////////////////////////////////////

#include "nstype.h"

#include <stdio.h>
#include "reqqueue.h"
#include "montrace.h"
#include "monsonar.h"
#include "monlogging.h"
#include "replicate.h"
#include "mlio.h"

extern CMonitor *Monitor;
extern CMonStats *MonStats;
extern CNodeContainer *Nodes;
extern CReplicate Replicator;
extern CNode *MyNode;
extern int MyPNID;

CExtDelProcessNsReq::CExtDelProcessNsReq (reqQueueMsg_t msgType, int pid,
                          int sockFd,
                          struct message_def *msg )
    : CExternalReq(msgType, pid, sockFd, msg)
{
    // Add eyecatcher sequence as a debugging aid
    memcpy(&eyecatcher_, "RqEA", 4);
}

CExtDelProcessNsReq::~CExtDelProcessNsReq()
{
    // Alter eyecatcher sequence as a debugging aid to identify deleted object
    memcpy(&eyecatcher_, "rQea", 4);
}

void CExtDelProcessNsReq::populateRequestString( void )
{
    char strBuf[MON_STRING_BUF_SIZE/2] = { 0 };

    snprintf( strBuf, sizeof(strBuf), 
              "ExtReq(%s) req #=%ld "
              "requester(name=%s/nid=%d/pid=%d/os_pid=%d/verifier=%d) "
              "target(name=%s/nid=%d/pid=%d/verifier=%d)"
            , CReqQueue::svcReqType[reqType_], getId()
            , msg_->u.request.u.del_process_ns.process_name
            , msg_->u.request.u.del_process_ns.nid
            , msg_->u.request.u.del_process_ns.pid
            , pid_
            , msg_->u.request.u.del_process_ns.verifier
            , msg_->u.request.u.del_process_ns.target_process_name
            , msg_->u.request.u.del_process_ns.target_nid
            , msg_->u.request.u.del_process_ns.target_pid
            , msg_->u.request.u.del_process_ns.target_verifier );
    requestString_.assign( strBuf );
}

void CExtDelProcessNsReq::performRequest()
{
    bool status = FAILURE;
    CProcess *process = NULL;
    CProcess *backup = NULL;

    const char method_name[] = "CExtDelProcessNsReq::performRequest";
    TRACE_ENTRY;

#if 0 // TODO
    // Record statistics (sonar counters)
    if (sonar_verify_state(SONAR_ENABLED | SONAR_MONITOR_ENABLED))
       MonStats->req_type_kill_Incr();
#endif

    // Trace info about request
    if (trace_settings & (TRACE_REQUEST | TRACE_PROCESS))
    {
        trace_printf( "%s@%d request #%ld: Delete, requester %s (%d, %d:%d), "
                      "target %s (%d, %d:%d)\n",
                      method_name, __LINE__, id_,
                      msg_->u.request.u.del_process_ns.process_name,
                      msg_->u.request.u.del_process_ns.nid,
                      msg_->u.request.u.del_process_ns.pid,
                      msg_->u.request.u.del_process_ns.verifier,
                      msg_->u.request.u.del_process_ns.target_process_name,
                      msg_->u.request.u.del_process_ns.target_nid,
                      msg_->u.request.u.del_process_ns.target_pid,
                      msg_->u.request.u.del_process_ns.target_verifier);
    }

    nid_ = msg_->u.request.u.del_process_ns.nid;
    verifier_ = msg_->u.request.u.del_process_ns.verifier;
    processName_ = msg_->u.request.u.del_process_ns.process_name;

    int       target_nid = -1;
    int       target_pid = -1;
    string    target_process_name;
    Verifier_t target_verifier = -1;
    
    target_nid = msg_->u.request.u.del_process_ns.target_nid;
    target_pid = msg_->u.request.u.del_process_ns.target_pid;
    target_process_name = (const char *) msg_->u.request.u.del_process_ns.target_process_name;
    target_verifier  = msg_->u.request.u.del_process_ns.target_verifier;

    if ( target_process_name.size() )
    { // find by name (check node state, don't check process state, not backup)
        process = Nodes->GetProcess( target_process_name.c_str()
                                   , target_verifier
                                   , true, false, false );
        if ( process &&
            (msg_->u.request.u.del_process_ns.target_nid == -1 ||
             msg_->u.request.u.del_process_ns.target_pid == -1))
        {
            backup = process->GetBackup ();
        }
    }
    else
    { // find by nid (check node state, don't check process state, backup is Ok)
        process = Nodes->GetProcess( target_nid
                                   , target_pid
                                   , target_verifier
                                   , true, false, true );
        backup = NULL;
    }


    if (process)
    {
        CNode * node = Nodes->GetLNode (process->GetNid())->GetNode();
        node->DelFromNameMap ( process );
        node->DelFromPidMap ( process );
        process->SetState (State_Stopped);
        // Replicate the exit to other nodes
        CReplExit *repl = new CReplExit(process->GetNid(),
                                        process->GetPid(),
                                        process->GetVerifier(),
                                        process->GetName(),
                                        process->IsAbended());
        Replicator.addItem(repl);
        process->SetDeletePending ( true );
        node->DeleteFromList( process );

        msg_->u.reply.type = ReplyType_DelProcessNs;
        msg_->u.reply.u.del_process_ns.nid = msg_->u.request.u.del_process_ns.target_nid;
        msg_->u.reply.u.del_process_ns.pid = msg_->u.request.u.del_process_ns.target_pid;
        msg_->u.reply.u.del_process_ns.verifier = msg_->u.request.u.del_process_ns.target_verifier;
        strncpy(msg_->u.reply.u.del_process_ns.process_name, msg_->u.request.u.del_process_ns.target_process_name, MAX_PROCESS_NAME);
        msg_->u.reply.u.del_process_ns.return_code = MPI_SUCCESS;
        status = SUCCESS;
    }
    else
    {
        if (trace_settings & (TRACE_REQUEST | TRACE_PROCESS))
        {
           trace_printf( "%s@%d - Kill %s (%d, %d:%d) -- can't find target process\n"
                       , method_name, __LINE__
                       , msg_->u.request.u.del_process_ns.target_process_name
                       , msg_->u.request.u.del_process_ns.target_nid
                       , msg_->u.request.u.del_process_ns.target_pid
                       , msg_->u.request.u.del_process_ns.target_verifier);
        }
    }

    if (status == FAILURE)
    {
        msg_->u.reply.type = ReplyType_DelProcessNs;
        msg_->u.reply.u.del_process_ns.nid = msg_->u.request.u.del_process_ns.target_nid;
        msg_->u.reply.u.del_process_ns.pid = msg_->u.request.u.del_process_ns.target_pid;
        msg_->u.reply.u.del_process_ns.verifier = msg_->u.request.u.del_process_ns.target_verifier;
        strncpy(msg_->u.reply.u.del_process_ns.process_name, msg_->u.request.u.del_process_ns.target_process_name, MAX_PROCESS_NAME);
        msg_->u.reply.u.del_process_ns.return_code = MPI_ERR_NAME;
        if (trace_settings & (TRACE_REQUEST | TRACE_PROCESS))
           trace_printf("%s@%d - unsuccessful\n", method_name, __LINE__);
    }

    // Send reply to requester
    monreply(msg_, sockFd_);

    TRACE_EXIT;
}
