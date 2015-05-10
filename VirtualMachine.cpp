#include <stdio.h>
#include <stdlib.h>
#include "VirtualMachine.h"
#include "Machine.h"
#include <vector>
#include <deque>

using namespace std;

const TVMMemoryPoolID VM_MEMORY_POOL_ID_SYSTEM = 0;

extern "C" {
///////////////////////// TCB Class Definition ///////////////////////////
class TCB
{
public:
    TCB(TVMThreadIDRef id, TVMThreadState thread_state, TVMThreadPriority thread_priority, TVMMemorySize stack_size, TVMThreadEntry entry_point, void *entry_params, TVMTick ticks_remaining) :
        id(id),
        thread_state(thread_state),
        thread_priority(thread_priority),
        stack_size(stack_size),
        entry_point(entry_point),
        entry_params(entry_params),
        ticks_remaining(ticks_remaining) {
            // need to use mem alloc for base, not new
            stack_base = new uint8_t[stack_size];
            // call_back_result = -1;
        }

    TVMThreadIDRef id;
    TVMThreadState thread_state;
    TVMThreadPriority thread_priority;
    TVMMemorySize stack_size;
    uint8_t *stack_base;
    TVMThreadEntry entry_point;
    void *entry_params;
    TVMTick ticks_remaining;
    SMachineContext machine_context; // for the context to switch to/from the thread
    int call_back_result;
};



///////////////////////// Mutex Class definition ///////////////////////////
class Mutex{
    public:
        Mutex(TVMMutexIDRef mutex_id_ref, TVMThreadIDRef owner_id_ref):
            mutex_id_ref(mutex_id_ref),
            owner_id_ref(owner_id_ref) {}

        TVMMutexIDRef mutex_id_ref;
        TVMThreadIDRef owner_id_ref;
        deque<TCB*> mutex_low_priority_queue;
        deque<TCB*> mutex_normal_priority_queue;
        deque<TCB*> mutex_high_priority_queue;
};


///////////////////////// Globals ///////////////////////////
#define VM_THREAD_PRIORITY_IDLE                  ((TVMThreadPriority)0x00)

vector<TCB*> thread_vector;
vector<Mutex*> mutex_vector;
deque<TCB*> low_priority_queue;
deque<TCB*> normal_priority_queue;
deque<TCB*> high_priority_queue;
vector<TCB*> sleep_vector;
TCB*        idle_thread;
TCB*        current_thread;
Mutex*      current_mutex;

volatile int timer;
TMachineSignalStateRef sigstate;

///////////////////////// Function Prototypes ///////////////////////////
TVMMainEntry VMLoadModule(const char *module);

///////////////////////// Utilities ///////////////////////////
void actual_removal(TCB* thread, deque<TCB*> &Q) {
    for (deque<TCB*>::iterator it=Q.begin(); it != Q.end(); ++it) {
        if (*it == thread) {
            Q.erase(it);
            break;
        }
    }
}

void determine_queue_and_push(TCB* thread) {
    if (thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
        low_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
        normal_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
        high_priority_queue.push_back(thread);
    }
}


void mutex_determine_queue_and_push(TCB* thread, Mutex* mutex) {
    if (thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
        low_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
        normal_priority_queue.push_back(thread);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
        high_priority_queue.push_back(thread);
    }
}

void determine_queue_and_remove(TCB *thread) {
    if (thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
        actual_removal(thread, low_priority_queue);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
        actual_removal(thread, normal_priority_queue);
    }
    else if (thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
        actual_removal(thread, high_priority_queue);
    }
}

void scheduler_action(deque<TCB*> &Q) {
    // set current thread to ready state if it was running
    if (current_thread->thread_state == VM_THREAD_STATE_RUNNING) {
        current_thread->thread_state = VM_THREAD_STATE_READY;
    }
    if (current_thread->thread_state == VM_THREAD_STATE_READY) {
        determine_queue_and_push(current_thread);
    }
    // need the ticks_remaining condition because threads that are waiting on file I/O are in wait state but shouldn't go to sleep_vector
    else if (current_thread->thread_state == VM_THREAD_STATE_WAITING && current_thread->ticks_remaining != 0) {
        sleep_vector.push_back(current_thread);
    }
    TCB* temp = current_thread;
    // set current to next and remove from Q
    current_thread = Q.front();
    Q.pop_front();
    // set current to running
    current_thread->thread_state = VM_THREAD_STATE_RUNNING;

    MachineContextSwitch(&(temp->machine_context), &(current_thread->machine_context));
}

void scheduler() {
    if (!high_priority_queue.empty()) {
        scheduler_action(high_priority_queue);
    }
    else if (!normal_priority_queue.empty()) {
        scheduler_action(normal_priority_queue);
    }
    else if (!low_priority_queue.empty()) {
        scheduler_action(low_priority_queue);
    }
    // schedule idle thread
    else {
        if (current_thread->thread_state == VM_THREAD_STATE_READY) {
            determine_queue_and_push(current_thread);
        }
        // need the ticks_remaining condition because threads that are waiting on file I/O are in wait state but shouldn't go to sleep_vector
        else if (current_thread->thread_state == VM_THREAD_STATE_WAITING && current_thread->ticks_remaining != 0) {
            sleep_vector.push_back(current_thread);
        }
        TCB* temp = current_thread;
        current_thread = idle_thread;
        current_thread->thread_state = VM_THREAD_STATE_RUNNING;
        thread_vector.push_back(current_thread);
        MachineContextSwitch(&(temp->machine_context), &(idle_thread->machine_context));
    }
}

void release(TVMMutexID mutex, deque<TCB*> &Q) {
    TCB* new_thread;
    if(!(Q.empty())) {
        new_thread = Q.front();;
        Q.pop_front();
        new_thread->thread_state = VM_THREAD_STATE_READY;
        high_priority_queue.push_back(new_thread);
        if(new_thread->thread_priority > current_thread->thread_priority) {
            scheduler();
        }
    }
}

///////////////////////// Callbacks ///////////////////////////
// for ref: typedef void (*TMachineAlarmCallback)(void *calldata);
void timerDecrement(void *calldata) {
    // decrements ticks for each sleeping thread
    for (int i = 0; i < sleep_vector.size(); i++) {
            sleep_vector[i]->ticks_remaining--;
            if (sleep_vector[i]->ticks_remaining ==  0) {
                sleep_vector[i]->thread_state = VM_THREAD_STATE_READY;
                determine_queue_and_push(sleep_vector[i]);
                sleep_vector.erase(sleep_vector.begin() + i);
                scheduler();
            }
    }
}

void SkeletonEntry(void *param) {
    MachineEnableSignals();
    TCB* temp = (TCB*)param;
    temp->entry_point(temp->entry_params);
    VMThreadTerminate(*(temp->id)); // This will allow you to gain control back if the ActualThreadEntry returns
}

void idleEntry(void *param) {
    while(1);
}

void MachineFileCallback(void* param, int result) {
    TCB* temp = (TCB*)param;
    temp->thread_state = VM_THREAD_STATE_READY;
    determine_queue_and_push(temp);
    temp->call_back_result = result;
    if ((current_thread->thread_state == VM_THREAD_STATE_RUNNING && current_thread->thread_priority < temp->thread_priority) || current_thread->thread_state != VM_THREAD_STATE_RUNNING) {
        scheduler();
    }
}

///////////////////////// VMThread Functions ///////////////////////////
TVMStatus VMStart(int tickms, TVMMemorySize heapsize, int machinetickms, TVMMemorySize sharedsize, int argc, char *argv[]) { //The time in milliseconds of the virtual machine tick is specified by the tickms parameter, the machine responsiveness is specified by the machinetickms.
    typedef void (*TVMMainEntry)(int argc, char* argv[]);
    TVMMainEntry VMMain;
    VMMain = VMLoadModule(argv[0]);
    if (VMMain != NULL) {
        // will need to pass in sharesize here vvvvv
        MachineInitialize(machinetickms); //The timeout parameter specifies the number of milliseconds the machine will sleep between checking for requests.
        MachineRequestAlarm(tickms*1000, timerDecrement, NULL); // NULL b/c passing data through global vars
        MachineEnableSignals();
        // create main_thread
        TCB* main_thread = new TCB((unsigned int *)0, VM_THREAD_STATE_RUNNING, VM_THREAD_PRIORITY_NORMAL, 0, NULL, NULL, 0);
        thread_vector.push_back(main_thread);
        current_thread = main_thread;
        // create idle_thread
        idle_thread = new TCB((unsigned int *)1, VM_THREAD_STATE_DEAD, VM_THREAD_PRIORITY_IDLE, 0x100000, NULL, NULL, 0);
        idle_thread->thread_state = VM_THREAD_STATE_READY;
        MachineContextCreate(&(idle_thread->machine_context), idleEntry, NULL, idle_thread->stack_base, idle_thread->stack_size);
        // VM_MEMORY_POOL_SYSTEM
        uint8_t* base = new uint8_t[heapsize];
        MemoryPool* main_pool = new MemoryPool(heapsize, VM_MEMORY_POOL_ID_SYSTEM, base);
        mem_pool_vector.push_back(main_pool);
        // call VMMain
        VMMain(argc, argv);
        return VM_STATUS_SUCCESS;
    }
    else {
        return VM_STATUS_FAILURE;
    }
}

TVMStatus VMThreadActivate(TVMThreadID thread) {
    MachineSuspendSignals(sigstate);
    TCB* actual_thread = thread_vector[thread];
    if (actual_thread->thread_state != VM_THREAD_STATE_DEAD) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        MachineContextCreate(&(actual_thread->machine_context), SkeletonEntry, actual_thread, actual_thread->stack_base, actual_thread->stack_size);
        actual_thread->thread_state = VM_THREAD_STATE_READY;
        determine_queue_and_push(actual_thread);
        if ((current_thread->thread_state == VM_THREAD_STATE_RUNNING && current_thread->thread_priority < actual_thread->thread_priority) || current_thread->thread_state != VM_THREAD_STATE_RUNNING) {
            scheduler();
        }
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadCreate(TVMThreadEntry entry, void *param, TVMMemorySize memsize, TVMThreadPriority prio, TVMThreadIDRef tid) {
    MachineSuspendSignals(sigstate);
    if (entry == NULL || tid == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }

    else {
        // NEED TO ALLOCATE SPACE FOR BASE OF THREADS FROM THE MAIN MEMPOOL, NOT USING NEW (look at constructor)
        TCB *new_thread = new TCB(tid, VM_THREAD_STATE_DEAD, prio, memsize, entry, param, 0);
        *(new_thread->id) = (TVMThreadID)thread_vector.size();
        thread_vector.push_back(new_thread);
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadDelete(TVMThreadID thread) {
    MachineSuspendSignals(sigstate);
    if (thread_vector[thread]->thread_state == VM_THREAD_STATE_DEAD) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        delete thread_vector[thread];
        thread_vector[thread] = NULL;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadID(TVMThreadIDRef threadref) {
    MachineSuspendSignals(sigstate);
    if (threadref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        threadref = current_thread->id;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadSleep(TVMTick tick){
    MachineSuspendSignals(sigstate);
    if (tick == VM_TIMEOUT_INFINITE) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        current_thread->ticks_remaining = tick;
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        // determine_queue_and_push();
        scheduler();
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadState(TVMThreadID thread, TVMThreadStateRef stateref) {
    MachineSuspendSignals(sigstate);
    if(thread == VM_THREAD_ID_INVALID) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if (stateref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        *stateref = thread_vector[thread]->thread_state;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

TVMStatus VMThreadTerminate(TVMThreadID thread) {
    MachineSuspendSignals(sigstate);
    if (thread_vector[thread]->thread_state == VM_THREAD_STATE_DEAD) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        thread_vector[thread]->thread_state = VM_THREAD_STATE_DEAD;
        determine_queue_and_remove(thread_vector[thread]);
        scheduler();
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
}

///////////////////////// VMFile Functions ///////////////////////////
TVMStatus VMFileClose(int filedescriptor) {
    MachineSuspendSignals(sigstate);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    MachineFileClose(filedescriptor, MachineFileCallback, current_thread);
    scheduler();
    if (current_thread->call_back_result != -1) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}



TVMStatus VMFileOpen(const char *filename, int flags, int mode, int *filedescriptor) {
    MachineSuspendSignals(sigstate);
    if (filename == NULL || filedescriptor == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else {
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        MachineFileOpen(filename, flags, mode, MachineFileCallback, current_thread);
        scheduler();
        *filedescriptor = current_thread->call_back_result;
        if (*filedescriptor != -1) {
            MachineResumeSignals(sigstate);
            return VM_STATUS_SUCCESS;
        }
        else {
            MachineResumeSignals(sigstate);
            return VM_STATUS_FAILURE;
        }
    }
}


TVMStatus VMFileWrite(int filedescriptor, void *data, int *length) {
    MachineSuspendSignals(sigstate);
    if (data == NULL || length == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    MachineFileWrite(filedescriptor, data, *length, MachineFileCallback, current_thread);
    scheduler();
    if (current_thread->call_back_result != -1) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}

TVMStatus VMFileRead(int filedescriptor, void *data, int *length) {
    MachineSuspendSignals(sigstate);
    if (data == NULL || length == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    MachineFileRead(filedescriptor, data, *length, MachineFileCallback, current_thread);
    scheduler();
    *length = current_thread->call_back_result;
    if(*length > 0) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}

TVMStatus VMFileSeek(int filedescriptor, int offset, int whence, int *newoffset) {
    MachineSuspendSignals(sigstate);
    current_thread->thread_state = VM_THREAD_STATE_WAITING;
    MachineFileSeek(filedescriptor, offset, whence, MachineFileCallback, current_thread);
    scheduler();
    if(newoffset != NULL) {
        *newoffset = current_thread->call_back_result;
        MachineResumeSignals(sigstate);
        return VM_STATUS_SUCCESS;
    }
    else {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
}


/////////////////////// VMMutex Functions ///////////////////////////
TVMStatus VMMutexCreate(TVMMutexIDRef mutexref) {
    MachineSuspendSignals(sigstate);
    if(mutexref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    *mutexref = (unsigned int)mutex_vector.size();
    current_mutex = new Mutex(mutexref, (TVMThreadIDRef)-1);
    mutex_vector.push_back(current_mutex);
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexDelete(TVMMutexID mutex) {
    MachineSuspendSignals(sigstate);
    if(mutex_vector[mutex] == NULL || mutex >= mutex_vector.size()) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(*(mutex_vector[mutex]->owner_id_ref) != -1) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    delete mutex_vector[mutex];
    mutex_vector[mutex] = NULL;
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}

TVMStatus VMMutexQuery(TVMMutexID mutex, TVMThreadIDRef ownerref) {
    MachineSuspendSignals(sigstate);
    if(ownerref == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_PARAMETER;
    }
    else if(mutex >= mutex_vector.size() || mutex_vector[mutex] == NULL) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(*(mutex_vector[mutex]->owner_id_ref) == -1) {
        MachineResumeSignals(sigstate);
        return VM_THREAD_ID_INVALID;
    }
    else {
        *ownerref = *(mutex_vector[mutex]->owner_id_ref);
    }
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;

}

TVMStatus VMMutexAcquire(TVMMutexID mutex, TVMTick timeout) {
    MachineSuspendSignals(sigstate);
    if(mutex > mutex_vector.size()) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    if(mutex_vector[mutex]->owner_id_ref == (TVMThreadIDRef)-1) {
        mutex_vector[mutex]->owner_id_ref = current_thread->id;
    }
    else if(timeout == VM_TIMEOUT_IMMEDIATE) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_FAILURE;
    }
    else if(timeout == VM_TIMEOUT_INFINITE) {
        current_thread->thread_state = VM_THREAD_STATE_WAITING;
        if(current_thread->thread_priority == VM_THREAD_PRIORITY_LOW) {
            mutex_vector[mutex]->mutex_low_priority_queue.push_back(current_thread);
        }
        else if(current_thread->thread_priority == VM_THREAD_PRIORITY_NORMAL) {
            mutex_vector[mutex]->mutex_normal_priority_queue.push_back(current_thread);
        }
        else if(current_thread->thread_priority == VM_THREAD_PRIORITY_HIGH) {
            mutex_vector[mutex]->mutex_high_priority_queue.push_back(current_thread);
        }
        scheduler();
    }
    else {
        determine_queue_and_push(current_thread);
        VMThreadSleep(timeout);
        if(mutex_vector[mutex]->owner_id_ref != current_thread->id) {
            MachineResumeSignals(sigstate);
            return VM_STATUS_FAILURE;
        }
    }
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;
}



TVMStatus VMMutexRelease(TVMMutexID mutex) {
    MachineSuspendSignals(sigstate);
    if(mutex_vector[mutex] == NULL || mutex >= mutex_vector.size()) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_ID;
    }
    else if(mutex_vector[mutex]->owner_id_ref == (TVMThreadIDRef)-1) {
        MachineResumeSignals(sigstate);
        return VM_STATUS_ERROR_INVALID_STATE;
    }
    else {
        if(!(mutex_vector[mutex]->mutex_high_priority_queue.empty())) {
            release(mutex, mutex_vector[mutex]->mutex_high_priority_queue);
        }
        else if(!(mutex_vector[mutex]->mutex_normal_priority_queue.empty())) {
            release(mutex, mutex_vector[mutex]->mutex_normal_priority_queue);
        }
        else if(!(mutex_vector[mutex]->mutex_low_priority_queue.empty())) {
            release(mutex, mutex_vector[mutex]->mutex_low_priority_queue);
        }
        else {
            mutex_vector[mutex]->owner_id_ref = (TVMThreadIDRef)-1;
        }
    }
    MachineResumeSignals(sigstate);
    return VM_STATUS_SUCCESS;

}


} // end extern C
