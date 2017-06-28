//===----RTLs/hsa/src/rtl.cpp - Target RTLs Implementation -------- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// RTL for hsa machine
// github: ashwinma (ashwinma@gmail.com)
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include <assert.h>
#include <cstdio>
#include <dlfcn.h>
#include <elf.h>
#include <ffi.h>
#include <gelf.h>
#include <list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <iostream>
#include <fstream>

#include "atmi_runtime.h"
#include "atmi_interop_hsa.h"

#include "omptarget.h"

#include "rtl.h"

// Static limit from previous version
//
// Limit this size to phisical units for now, this should be way bigger
/* Use this to decide total teams: active groups * number of compute unit */
#define TEAMS_ABSOLUTE_LIMIT 512 //4*8 /* omptx limit (must match teamsAbsoluteLimit) */
// Let's don't simulate SIMD yet, WAVEFRONTSIZE X THREAD_ABSOLUTE_LIMIT is the local group size
#define THREAD_ABSOLUTE_LIMIT 1024// 2 /* omptx limit (must match threadAbsoluteLimit) */

// max number of blocks depend on the kernel we are executing - pick default here
#define MAX_NUM_TEAMS TEAMS_ABSOLUTE_LIMIT
#define WAVEFRONTSIZE 64

#define MAX_NUM_WAVES   (MAX_NUM_TEAMS * THREAD_ABSOLUTE_LIMIT / WAVEFRONTSIZE)
#define MAX_NUM_THREADS MAX_NUM_WAVES * WAVEFRONTSIZE

#ifdef OMPTHREAD_IS_WAVEFRONT
  // assume here one OpenMP thread per HSA wavefront
  #define MAX_NUM_OMP_THREADS MAX_NUM_WAVES
#else
  // assume here one OpenMP thread per HSA thread
  #define MAX_NUM_OMP_THREADS MAX_NUM_THREADS
#endif

#ifndef TARGET_NAME
#define TARGET_NAME AMDHSA
#endif
#define GETNAME2(name) #name
#define GETNAME(name) GETNAME2(name)
#define DP(...) DEBUGP("Target " GETNAME(TARGET_NAME) " RTL",__VA_ARGS__)
#ifdef OMPTARGET_DEBUG
#define check(msg, status) \
  if (status != ATMI_STATUS_SUCCESS) { \
    /* fprintf(stderr, "[%s:%d] %s failed.\n", __FILE__, __LINE__, #msg);*/ \
    DP(#msg" failed\n") \
    /*assert(0);*/ \
  } else { \
    /* fprintf(stderr, "[%s:%d] %s succeeded.\n", __FILE__, __LINE__, #msg); */ \
    DP(#msg" succeeded\n") \
  }
#else
#define check(msg, status) \
{}
#endif

/// Keep entries table per device
struct FuncOrGblEntryTy {
  __tgt_target_table Table;
  std::vector<__tgt_offload_entry> Entries;
};

enum ExecutionModeType {
  SPMD,
  GENERIC,
  NONE
};

typedef void * ATMIfunction;
/// Use a single entity to encode a kernel and a set of flags
struct KernelTy {
  ATMIfunction Func;

  // execution mode of kernel
  // 0 - SPMD mode (without master warp)
  // 1 - Generic mode (with master warp)
  int8_t ExecutionMode;

  KernelTy(ATMIfunction _Func, int8_t _ExecutionMode)
      : Func(_Func), ExecutionMode(_ExecutionMode) {}
};

/// List that contains all the kernels.
/// FIXME: we may need this to be per device and per library.
std::list<KernelTy> KernelsList;

/// Class containing all the device information
class RTLDeviceInfoTy {
  std::vector<FuncOrGblEntryTy> FuncGblEntries;
  int NumberOfiGPUs;
  int NumberOfdGPUs;
public:
  int NumberOfDevices;

  // GPU devices
  atmi_machine_t *Machine;
  std::vector<atmi_place_t> GPUPlaces;
  std::vector<hsa_agent_t> HSAAgents;

  // Device properties
  std::vector<int> GroupsPerDevice;
  std::vector<int> ThreadsPerGroup;
  std::vector<int> WavefrontSize;

  // OpenMP properties
  std::vector<int> NumTeams;
  std::vector<int> NumThreads;

  // OpenMP Environment properties
  int EnvNumTeams;
  int EnvTeamLimit;

  //static int EnvNumThreads;
  static const int HardTeamLimit = 1<<16; // 64k
  static const int HardThreadLimit = 1024;
  static const int DefaultNumTeams = 128;
  static const int DefaultNumThreads = 128;

  // Record entry point associated with device
  void addOffloadEntry(int device_id, __tgt_offload_entry entry ){
    assert( device_id < (int)FuncGblEntries.size() && "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];

    E.Entries.push_back(entry);
  }

  // Return true if the entry is associated with device
  bool findOffloadEntry(int device_id, void *addr){
    assert( device_id < (int)FuncGblEntries.size() && "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];

    for(unsigned i=0; i<E.Entries.size(); ++i){
      if(E.Entries[i].addr == addr)
        return true;
    }

    return false;
  }

  // Return the pointer to the target entries table
  __tgt_target_table *getOffloadEntriesTable(int device_id){
    assert( device_id < (int)FuncGblEntries.size() && "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];

    unsigned size = E.Entries.size();

    // Table is empty
    if(!size)
      return 0;

    __tgt_offload_entry *begin = &E.Entries[0];
    __tgt_offload_entry *end = &E.Entries[size-1];

    // Update table info according to the entries and return the pointer
    E.Table.EntriesBegin = begin;
    E.Table.EntriesEnd = ++end;

    return &E.Table;
  }

  // Clear entries table for a device
  void clearOffloadEntriesTable(int device_id){
    assert( device_id < (int)FuncGblEntries.size() && "Unexpected device id!");
    FuncOrGblEntryTy &E = FuncGblEntries[device_id];
    E.Entries.clear();
    E.Table.EntriesBegin = E.Table.EntriesEnd = 0;
  }

  RTLDeviceInfoTy() {
    DP("Start initializing HSA-ATMI\n");
    atmi_status_t err = atmi_init(ATMI_DEVTYPE_ALL);
    if (err != ATMI_STATUS_SUCCESS) {
      DP("Error when initializing HSA-ATMI\n");
      return;
    }
    atmi_machine_t *machine = atmi_machine_get_info();
    NumberOfiGPUs = machine->device_count_by_type[ATMI_DEVTYPE_iGPU]; 
    NumberOfdGPUs = machine->device_count_by_type[ATMI_DEVTYPE_dGPU]; 
    NumberOfDevices = machine->device_count_by_type[ATMI_DEVTYPE_GPU]; 
    DP("HSA Device GPU Count: %d\n", NumberOfDevices);

    Machine = machine;

    // Init the device info
    FuncGblEntries.resize(NumberOfDevices);
    GPUPlaces.resize(NumberOfDevices);
    HSAAgents.resize(NumberOfDevices);
    ThreadsPerGroup.resize(NumberOfDevices);
    GroupsPerDevice.resize(NumberOfDevices);
    WavefrontSize.resize(NumberOfDevices);
    NumTeams.resize(NumberOfDevices);
    NumThreads.resize(NumberOfDevices);

    for (int i =0; i<NumberOfDevices; i++) {
      ThreadsPerGroup[i]=RTLDeviceInfoTy::DefaultNumThreads;
      GroupsPerDevice[i]=RTLDeviceInfoTy::DefaultNumTeams;

      DP("Device %d: Initial groupsPerDevice %d & threadsPerGroup %d\n",
          i, GroupsPerDevice[i], ThreadsPerGroup[i]);

      //ATMI API to get gpu place
      GPUPlaces[i] = (atmi_place_t)ATMI_PLACE_GPU(0, i);

      //ATMI API to get HSA agent
      err = atmi_interop_hsa_get_agent(GPUPlaces[i], &(HSAAgents[i]));

      check("Get HSA agents", err);
    }

    // Get environment variables regarding teams
    char *envStr = getenv("OMP_TEAM_LIMIT");
    if (envStr) {
      // OMP_TEAM_LIMIT has been set
      EnvTeamLimit = std::stoi(envStr);
      DP("Parsed OMP_TEAM_LIMIT=%d\n", EnvTeamLimit);
    } else {
      EnvTeamLimit = -1;
    }
    envStr = getenv("OMP_NUM_TEAMS");
    if (envStr) {
      // OMP_NUM_TEAMS has been set
      EnvNumTeams = std::stoi(envStr);
      DP("Parsed OMP_NUM_TEAMS=%d\n", EnvNumTeams);
    } else {
      EnvNumTeams = -1;
    }
  }

  ~RTLDeviceInfoTy(){
    DP("Finalizing the HSA-ATMI DeviceInfo.\n");
    atmi_finalize();

    // Free devices allocated entry
    for(unsigned i=0; i<FuncGblEntries.size(); ++i ) {
      for(unsigned j=0; j<FuncGblEntries[i].Entries.size(); ++j ) {
        if(FuncGblEntries[i].Entries[j].addr) {
          KernelTy *KernelInfo = (KernelTy *)FuncGblEntries[i].Entries[j].addr;
          if (KernelInfo->Func) {
            DP("Free address %016llx.\n",(long long unsigned)(Elf64_Addr)KernelInfo->Func);
            free(KernelInfo->Func);
          }
        }
      }
    }
  }
};

static RTLDeviceInfoTy DeviceInfo;

#ifdef __cplusplus
extern "C" {
#endif

int32_t __tgt_rtl_is_valid_binary(__tgt_device_image *image) {

  // Is the library version incompatible with the header file?
  if (elf_version(EV_CURRENT) == EV_NONE) {
    DP("Incompatible ELF library!\n");
    return 0;
  }

  char *img_begin = (char *)image->ImageStart;
  char *img_end = (char *)image->ImageEnd;
  size_t img_size = img_end - img_begin;

  // Obtain elf handler
  Elf *e = elf_memory(img_begin, img_size);
  if (!e) {
    DP("Unable to get ELF handle: %s!\n", elf_errmsg(-1));
    return 0;
  }

  // Check if ELF is the right kind.
  if (elf_kind(e) != ELF_K_ELF) {
    DP("Unexpected ELF type!\n");
    return 0;
  }
  Elf64_Ehdr *eh64 = elf64_getehdr(e);
  Elf32_Ehdr *eh32 = elf32_getehdr(e);

  if (!eh64 && !eh32) {
    DP("Unable to get machine ID from ELF file!\n");
    elf_end(e);
    return 0;
  }

  uint16_t MachineID;
  if (eh64 && !eh32)
    MachineID = eh64->e_machine;
  else if (eh32 && !eh64)
    MachineID = eh32->e_machine;
  else {
    DP("Ambiguous ELF header!\n");
    elf_end(e);
    return 0;
  }

  elf_end(e);

  switch(MachineID) {
    // old brig file in HSA 1.0P
    case 0:
    // brig file in HSAIL path
    case 44890:
    case 44891:
      break;
    // amdgcn
    case 224:
      break;
    default:
      DP("Unsupported machine ID found: %d\n", MachineID);
      return 0;
  }

  return 1;
}

int __tgt_rtl_number_of_devices(){
  return DeviceInfo.NumberOfDevices;
}

int32_t __tgt_rtl_init_device(int device_id){
  hsa_status_t err;

  // this is per device id init
  DP("Initialize the device id: %d\n", device_id);

  hsa_agent_t  &agent = DeviceInfo.HSAAgents[device_id];

  //DP("Initialize the device, GroupsPerDevice limit: %d\n", TEAMS_ABSOLUTE_LIMIT);
  //DP("Initialize the device, ThreadsPerGroup limit: %d\n", MAX_NUM_OMP_THREADS);

  // get the global one from device side as we use static structure
  int GroupsLimit = TEAMS_ABSOLUTE_LIMIT;
  int ThreadsLimit = MAX_NUM_OMP_THREADS/TEAMS_ABSOLUTE_LIMIT;

  // Get number of Compute Unit
  uint32_t compute_unit = 0;
  err = hsa_agent_get_info(agent, (hsa_agent_info_t)HSA_AMD_AGENT_INFO_COMPUTE_UNIT_COUNT, &compute_unit);
  if (err == HSA_STATUS_SUCCESS) {
    DP("Queried compute unite size: %d\n", compute_unit);
    DeviceInfo.GroupsPerDevice[device_id] = compute_unit;
  }
  else {
    DP("Default compute unite size: %d\n", GroupsLimit  );
    DeviceInfo.GroupsPerDevice[device_id] = GroupsLimit;
  }

  if ((DeviceInfo.GroupsPerDevice[device_id] > GroupsLimit) ||
      (DeviceInfo.GroupsPerDevice[device_id] == 0)) {
    DeviceInfo.GroupsPerDevice[device_id]=GroupsLimit;
  }

  // Get thread limit
  //
  //uint32_t workgroup_max_size = 0;
  //err = hsa_agent_get_info(agent, HSA_AGENT_INFO_WORKGROUP_MAX_SIZE, &workgroup_max_size);

  uint16_t workgroup_max_dim[3];
  err = hsa_agent_get_info(agent, HSA_AGENT_INFO_WORKGROUP_MAX_DIM, &workgroup_max_dim);
  if (err == HSA_STATUS_SUCCESS) {
    DP("Queried thread limit: %d\n", (int)workgroup_max_dim[0]);
    DeviceInfo.ThreadsPerGroup[device_id] = workgroup_max_dim[0];
  }
  else {
    DP("Default thread limit: %d\n", ThreadsLimit  );
    DeviceInfo.ThreadsPerGroup[device_id] = ThreadsLimit;
  }

  if ((DeviceInfo.ThreadsPerGroup[device_id] > ThreadsLimit) ||
      DeviceInfo.ThreadsPerGroup[device_id] == 0) {
    DeviceInfo.ThreadsPerGroup[device_id]=ThreadsLimit;
  }

  // Get wavefront size
  uint32_t wavefront_size = 0;

  err = hsa_agent_get_info(agent, HSA_AGENT_INFO_WAVEFRONT_SIZE, &wavefront_size);
  if (err == HSA_STATUS_SUCCESS) {
    DP("Queried wavefront size: %d\n", wavefront_size);
    DeviceInfo.WavefrontSize[device_id] = wavefront_size;
  }
  else {
    DP("Default wavefront size: %d\n", WAVEFRONTSIZE );
    DeviceInfo.WavefrontSize[device_id] = WAVEFRONTSIZE;
  }

  DP("Device %d: default limit for groupsPerDevice %d & threadsPerGroup %d\n",
      device_id,
      DeviceInfo.GroupsPerDevice[device_id],
      DeviceInfo.ThreadsPerGroup[device_id]);

  DP("Device %d: total threads %d x %d = %d\n",
      device_id,
      DeviceInfo.ThreadsPerGroup[device_id],
      DeviceInfo.GroupsPerDevice[device_id],
      DeviceInfo.GroupsPerDevice[device_id]*DeviceInfo.ThreadsPerGroup[device_id]);

  // Adjust teams to the env variables
  if (DeviceInfo.EnvTeamLimit > 0 &&
      DeviceInfo.GroupsPerDevice[device_id] > DeviceInfo.EnvTeamLimit) {
    DeviceInfo.GroupsPerDevice[device_id] = DeviceInfo.EnvTeamLimit;
    DP("Capping max groups per device to OMP_TEAM_LIMIT=%d\n",
        DeviceInfo.EnvTeamLimit);
  }

  // Set default number of teams
  if (DeviceInfo.EnvNumTeams > 0) {
    DeviceInfo.NumTeams[device_id] = DeviceInfo.EnvNumTeams;
    DP("Default number of teams set according to environment %d\n",
        DeviceInfo.EnvNumTeams);
  } else {
    DeviceInfo.NumTeams[device_id] = RTLDeviceInfoTy::DefaultNumTeams;
    DP("Default number of teams set according to library's default %d\n",
        RTLDeviceInfoTy::DefaultNumTeams);
  }
  if (DeviceInfo.NumTeams[device_id] > DeviceInfo.GroupsPerDevice[device_id]) {
    DeviceInfo.NumTeams[device_id] = DeviceInfo.GroupsPerDevice[device_id];
    DP("Default number of teams exceeds device limit, capping at %d\n",
        DeviceInfo.GroupsPerDevice[device_id]);
  }

  // Set default number of threads
  DeviceInfo.NumThreads[device_id] = RTLDeviceInfoTy::DefaultNumThreads;
  DP("Default number of threads set according to library's default %d\n",
          RTLDeviceInfoTy::DefaultNumThreads);
  if (DeviceInfo.NumThreads[device_id] >
      DeviceInfo.ThreadsPerGroup[device_id]) {
    DeviceInfo.NumTeams[device_id] = DeviceInfo.ThreadsPerGroup[device_id];
    DP("Default number of threads exceeds device limit, capping at %d\n",
        DeviceInfo.ThreadsPerGroup[device_id]);
  }

  return OFFLOAD_SUCCESS ;
}

__tgt_target_table *__tgt_rtl_load_binary(int32_t device_id, __tgt_device_image *image){
  size_t img_size = (char*) image->ImageEnd - (char*) image->ImageStart;

  DeviceInfo.clearOffloadEntriesTable(device_id);
  // TODO: is BRIG even required to be supported? Can we assume AMDGCN only?
  int useBrig = 0;

  // We do not need to set the ELF version because the caller of this function
  // had to do that to decide the right runtime to use

  // Obtain elf handler and do an extra check
  {
    Elf *elfP = elf_memory ((char*)image->ImageStart, img_size);
    if(!elfP){
      DP("Unable to get ELF handle: %s!\n", elf_errmsg(-1));
      return 0;
    }

    if( elf_kind(elfP) !=  ELF_K_ELF){
      DP("Invalid Elf kind!\n");
      elf_end(elfP);
      return 0;
    }

    uint16_t MachineID;
    {
      Elf64_Ehdr *eh64 = elf64_getehdr(elfP);
      Elf32_Ehdr *eh32 = elf32_getehdr(elfP);
      if (eh64 && !eh32)
        MachineID = eh64->e_machine;
      else if (eh32 && !eh64)
        MachineID = eh32->e_machine;
      else{
        printf("Ambiguous ELF header!\n");
        return 0;
      }
    }

    switch(MachineID) {
      // old brig file in HSA 1.0P
      case 0:
      // brig file in HSAIL path
      case 44890:
      case 44891:
        {
          useBrig = 1;
        };
        break;
      case 224:
        // do nothing, amdgcn
        break;
      default:
        DP("Unsupported machine ID found: %d\n", MachineID);
        elf_end(elfP);
        return 0;
    }

    DP("Machine ID found: %d\n", MachineID);
    // Close elf
    elf_end(elfP);
  }

   atmi_platform_type_t platform = ( useBrig ? BRIG : AMDGCN );
   void *new_img = malloc(img_size);
   memcpy(new_img, image->ImageStart, img_size);
   atmi_status_t err = atmi_module_register_from_memory((void **)&new_img, &img_size, &platform, 1);

   free(new_img);
   check("Module registering", err);
   new_img = NULL;

   DP("ATMI module successfully loaded!\n");

  // TODO: Check with Guansong to understand the below comment more thoroughly. 
  // Here, we take advantage of the data that is appended after img_end to get
  // the symbols' name we need to load. This data consist of the host entries
  // begin and end as well as the target name (see the offloading linker script
  // creation in clang compiler).

   // Find the symbols in the module by name. The name can be obtain by
   // concatenating the host entry name with the target name

   __tgt_offload_entry *HostBegin = image->EntriesBegin;
   __tgt_offload_entry *HostEnd   = image->EntriesEnd;

   for( __tgt_offload_entry *e = HostBegin; e != HostEnd; ++e) {

     if( !e->addr ){
       // FIXME: Probably we should fail when something like this happen, the
       // host should have always something in the address to uniquely identify
       // the target region.
       DP("Analyzing host entry '<null>' (size = %lld)...\n",
           (unsigned long long)e->size);

       __tgt_offload_entry entry = *e;
       DeviceInfo.addOffloadEntry(device_id, entry);
       continue;
     }

     if( e->size ){
       __tgt_offload_entry entry = *e;

       void *varptr;
       uint32_t varsize;
       atmi_mem_place_t place = ATMI_MEM_PLACE_GPU_MEM(0, device_id, 0);
       err = atmi_interop_hsa_get_symbol_info(place, e->name, &varptr, &varsize);

       if (err != ATMI_STATUS_SUCCESS) {
         DP("Loading global '%s' (Failed)\n", e->name);
         return NULL;
       }

       if (varsize != e->size) {
         DP("Loading global '%s' - size mismatch (%u != %lu)\n", e->name,
             varsize, e->size);
         return NULL;
       }

       DP("Entry point " DPxMOD " maps to global %s (" DPxMOD ")\n",
           DPxPTR(e - HostBegin), e->name, DPxPTR(varptr));
       entry.addr = (void *)varptr;

       DeviceInfo.addOffloadEntry(device_id, entry);

       continue;
     }

     // TODO: we malloc, need free
     char *name_buffer = (char *)malloc(strlen(e->name) + 1);
     DP("Malloc address %016llx.\n",(long long unsigned)(Elf64_Addr)name_buffer);

     memcpy(name_buffer, e->name, strlen(e->name));
     name_buffer[strlen(e->name)] = 0;
     sprintf(name_buffer, "%s", e->name);
     DP("to find the kernel name: %s size: %lu\n", name_buffer, strlen(e->name));

     int8_t ExecModeVal = ExecutionModeType::SPMD;
     std::string ExecModeNameStr (e->name);
     ExecModeNameStr += "_exec_mode";
     const char *ExecModeName = ExecModeNameStr.c_str();

     void *ExecModePtr;
     uint32_t varsize;
     atmi_mem_place_t place = ATMI_MEM_PLACE_GPU_MEM(0, device_id, 0);
     err = atmi_interop_hsa_get_symbol_info(place, ExecModeName,
                                                        &ExecModePtr, &varsize);
     if (err == ATMI_STATUS_SUCCESS) {
       if ((size_t)varsize != sizeof(int8_t)) {
         DP("Loading global exec_mode '%s' - size mismatch (%u != %lu)\n",
            ExecModeName, varsize, sizeof(int8_t));
         return NULL;
       }

       err = atmi_memcpy(&ExecModeVal, ExecModePtr, (size_t) varsize);
       if (err != ATMI_STATUS_SUCCESS) {
         DP("Error when copying data from device to host. Pointers: "
            "host = " DPxMOD ", device = " DPxMOD ", size = %u\n",
            DPxPTR(&ExecModeVal), DPxPTR(ExecModePtr), varsize);
         return NULL;
       }
       DP("After loading global for %s ExecModeVal = %d\n",ExecModeName,ExecModeVal);

       if (ExecModeVal < 0 || ExecModeVal > 1) {
         DP("Error wrong exec_mode value specified in HSA code object file: %d\n",
            ExecModeVal);
         return NULL;
       }
     } else {
       DP("Loading global exec_mode '%s' - symbol missing, using default value "
           "SPMD (0)\n", ExecModeName);
     }

     KernelsList.push_back(KernelTy((void *)name_buffer, ExecModeVal));

     __tgt_offload_entry entry = *e;
     entry.addr = (void *)&KernelsList.back();
     DeviceInfo.addOffloadEntry(device_id, entry);
     DP("Entry point %ld maps to %s\n", e - HostBegin, e->name);
   }

   {// send device environment here
     omptarget_device_environmentTy device_env;

     device_env.num_devices = DeviceInfo.NumberOfDevices;
     device_env.device_num = device_id;

#ifdef OMPTARGET_DEBUG
     if (getenv("DEVICE_DEBUG"))
       device_env.debug_mode = 1;
     else
       device_env.debug_mode = 0;
#endif

     const char * device_env_Name="omptarget_device_environment";
     void *device_env_Ptr;
     uint32_t varsize;

     atmi_mem_place_t place = ATMI_MEM_PLACE_GPU_MEM(0, device_id, 0);
     err = atmi_interop_hsa_get_symbol_info(place, device_env_Name,
         &device_env_Ptr, &varsize);

     if (err == ATMI_STATUS_SUCCESS) {
       if ((size_t)varsize != sizeof(device_env)) {
         DP("Global device_environment '%s' - size mismatch (%u != %lu)\n",
             device_env_Name, varsize, sizeof(int32_t));
         return NULL;
       }

       err = atmi_memcpy(device_env_Ptr, &device_env, varsize);
       if (err != ATMI_STATUS_SUCCESS) {
         DP("Error when copying data from host to device. Pointers: "
             "host = " DPxMOD ", device = " DPxMOD ", size = %u\n",
             DPxPTR(&device_env), DPxPTR(device_env_Ptr), varsize);
         return NULL;
       }

       DP("Sending global device environment %lu bytes\n", (size_t)varsize);
     } else {
       DP("Finding global device environment '%s' - symbol missing.\n", device_env_Name);
       // no need to return NULL, consider this is a not a device debug build.
       //return NULL;
     }
   }

   return DeviceInfo.getOffloadEntriesTable(device_id);
}

void *__tgt_rtl_data_alloc(int device_id, int64_t size){
  void *ptr = NULL;
    assert(device_id < (int)DeviceInfo.Machine->device_count_by_type[ATMI_DEVTYPE_GPU] && "Device ID too large");
    atmi_mem_place_t place = ATMI_MEM_PLACE_GPU_MEM(0, device_id, 0);
    atmi_status_t err = atmi_malloc(&ptr, size, place);
    DP("Tgt alloc data %ld bytes, (tgt:%016llx).\n", size, (long long unsigned)(Elf64_Addr)ptr);
    ptr = (err == ATMI_STATUS_SUCCESS) ? ptr : NULL;
    return ptr;
}

int32_t __tgt_rtl_data_submit(int device_id, void *tgt_ptr, void *hst_ptr, int64_t size){
    atmi_status_t err; 
    assert(device_id < (int)DeviceInfo.Machine->device_count_by_type[ATMI_DEVTYPE_GPU] && "Device ID too large");
    DP("Submit data %ld bytes, (hst:%016llx) -> (tgt:%016llx).\n", size, (long long unsigned)(Elf64_Addr)hst_ptr, (long long unsigned)(Elf64_Addr)tgt_ptr);
    err = atmi_memcpy(tgt_ptr, hst_ptr, (size_t) size);
    if (err != ATMI_STATUS_SUCCESS) {
        DP("Error when copying data from host to device. Pointers: "
                "host = 0x%016lx, device = 0x%016lx, size = %lld\n",
                (Elf64_Addr)hst_ptr, (Elf64_Addr)tgt_ptr, (unsigned long long)size);
        return OFFLOAD_FAIL;
    }
    return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_retrieve(int device_id, void *hst_ptr, void *tgt_ptr, int64_t size){
    assert(device_id < (int)DeviceInfo.Machine->device_count_by_type[ATMI_DEVTYPE_GPU] && "Device ID too large");
    atmi_status_t err;
    DP("Retrieve data %ld bytes, (tgt:%016llx) -> (hst:%016llx).\n", size, (long long unsigned)(Elf64_Addr)tgt_ptr, (long long unsigned)(Elf64_Addr)hst_ptr);
    err = atmi_memcpy(hst_ptr, tgt_ptr, (size_t) size);
    if (err != ATMI_STATUS_SUCCESS) {
        DP("Error when copying data from device to host. Pointers: "
                "host = 0x%016lx, device = 0x%016lx, size = %lld\n",
                (Elf64_Addr)hst_ptr, (Elf64_Addr)tgt_ptr, (unsigned long long)size);
        return OFFLOAD_FAIL;
    }
    DP("DONE Retrieve data %ld bytes, (tgt:%016llx) -> (hst:%016llx).\n", size, (long long unsigned)(Elf64_Addr)tgt_ptr, (long long unsigned)(Elf64_Addr)hst_ptr);
    return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_data_delete(int device_id, void* tgt_ptr) {
    assert(device_id < (int)DeviceInfo.Machine->device_count_by_type[ATMI_DEVTYPE_GPU] && "Device ID too large");
    atmi_status_t err;
    DP("Tgt free data (tgt:%016llx).\n", (long long unsigned)(Elf64_Addr)tgt_ptr);
    err = atmi_free(tgt_ptr);
    if (err != ATMI_STATUS_SUCCESS) {
        DP("Error when freeing CUDA memory\n");
        return OFFLOAD_FAIL;
    }
    return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_team_region(int32_t device_id,
    void *tgt_entry_ptr, void **tgt_args, int32_t arg_count,
    int32_t team_num, int32_t thread_limit,
    uint64_t loop_tripcount) {

  //atmi_status_t err;

  int32_t arg_num = arg_count; // not sure why omptarget includes a last NULL arg

  // Set the context we are using
  // update thread limit content in gpu memory if un-initialized or specified from host

  /*
   * Set limit based on ThreadsPerGroup and GroupsPerDevice
   */
  int threadsPerGroup;

  DP("Run target team region thread_limit %d\n", thread_limit);

  // All args are references.
  std::vector<void *> args(arg_num);
  std::vector<size_t> arg_sizes(arg_num);

  DP("Arg_num: %d\n", arg_num);
  for (int32_t i = 0; i < arg_num; ++i) {
    args[i] = &tgt_args[i];
    arg_sizes[i] = sizeof(void *);
    DP("Arg[%d]: %p, size: %lu\n", i, tgt_args[i], sizeof(tgt_args[i]));
  }
  arg_sizes[arg_num- 1] = sizeof(int);

  KernelTy *KernelInfo = (KernelTy *)tgt_entry_ptr;

  if (thread_limit > 0) {
    threadsPerGroup = thread_limit;
    DP("Setting threads per block to requested %d\n", thread_limit);
  } else {
    threadsPerGroup = DeviceInfo.NumThreads[device_id];
    if (KernelInfo->ExecutionMode == GENERIC) {
      // Leave room for the master warp which will be added below.
      threadsPerGroup -= DeviceInfo.WavefrontSize[device_id];
      DP("Preparing %d threads\n", threadsPerGroup);
    }
    DP("Setting threads per block to default %d\n",
        DeviceInfo.NumThreads[device_id]);
  }

  DP("Preparing %d threads\n", threadsPerGroup);

  // Add master warp if necessary
  if (KernelInfo->ExecutionMode == GENERIC) {
    threadsPerGroup += DeviceInfo.WavefrontSize[device_id];
    DP("Adding master warp: +%d threads\n", DeviceInfo.WavefrontSize[device_id]);
    DP("Preparing %d threads\n", threadsPerGroup);
  }

  if (threadsPerGroup > DeviceInfo.ThreadsPerGroup[device_id]) {
    threadsPerGroup = DeviceInfo.ThreadsPerGroup[device_id];
    DP("Threads per group capped at device limit %d\n",
        DeviceInfo.ThreadsPerGroup[device_id]);
  }

  DP("Preparing %d threads\n", threadsPerGroup);

/*
  int kernel_limit;
  err = cuFuncGetAttribute(&kernel_limit,
      CU_FUNC_ATTRIBUTE_MAX_THREADS_PER_BLOCK, KernelInfo->Func);
  if (err == CUDA_SUCCESS) {
    if (kernel_limit < threadsPerGroup) {
      threadsPerGroup = kernel_limit;
      DP("Threads per block capped at kernel limit %d\n", kernel_limit);
    }
  }
  DP("Preparing %d threads\n", threadsPerGroup);
*/

  unsigned num_groups;
  if (team_num <= 0) {
    if (loop_tripcount > 0 && DeviceInfo.EnvNumTeams < 0) {
      if (KernelInfo->ExecutionMode == SPMD) {
        // round up to the nearest integer
        num_groups = ((loop_tripcount - 1) / threadsPerGroup) + 1;
      } else {
        num_groups = loop_tripcount;
      }
      DP("Using %d teams due to loop trip count %" PRIu64 " and number of "
          "threads per block %d\n", num_groups, loop_tripcount,
          threadsPerGroup);
    } else {
      num_groups = DeviceInfo.NumTeams[device_id];
      DP("Using default number of teams %d\n", DeviceInfo.NumTeams[device_id]);
    }
  } else if (team_num > DeviceInfo.GroupsPerDevice[device_id]) {
    num_groups = DeviceInfo.GroupsPerDevice[device_id];
    DP("Capping number of teams to team limit %d\n",
        DeviceInfo.GroupsPerDevice[device_id]);
  } else {
    num_groups = team_num;
    DP("Using requested number of teams %d\n", team_num);
  }

  char *kernel_name = (char *)KernelInfo->Func;
  // Run on the device.
  DP("Launch kernel %s with %d blocks and %d threads\n", kernel_name, num_groups,
     threadsPerGroup);

  atmi_kernel_t kernel;
  const int GPU_IMPL = 42;
  atmi_kernel_create_empty(&kernel, arg_num, &arg_sizes[0]);
  atmi_kernel_add_gpu_impl(kernel, kernel_name, GPU_IMPL);

  ATMI_LPARM_1D(lparm, num_groups*threadsPerGroup);
  lparm->groupDim[0] = threadsPerGroup;
  lparm->synchronous = ATMI_TRUE;
  lparm->groupable = ATMI_FALSE;
  // TODO: CUDA does not look like being synchronous.
  lparm->kernel_id = GPU_IMPL;
  lparm->place = ATMI_PLACE_GPU(0, device_id);
  atmi_task_launch(lparm, kernel, &args[0]);

  DP("Kernel %s completed\n", kernel_name);
  // FIXME: if kernel launch is asynchronous, then kernel should be added to a
  // vector in deviceinfo and released in the destructor
  atmi_kernel_release(kernel);

  return OFFLOAD_SUCCESS;
}

int32_t __tgt_rtl_run_target_region(int32_t device_id, void *tgt_entry_ptr,
    void **tgt_args, int32_t arg_num)
{
  // use one team and one thread
  // fix thread num
  int32_t team_num = 1;
  int32_t thread_limit = 0; // use default
  return __tgt_rtl_run_target_team_region(device_id,
      tgt_entry_ptr, tgt_args, arg_num, team_num, thread_limit, 0);
}

#ifdef __cplusplus
}
#endif

