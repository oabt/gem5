/*
 * Copyright (c) 2003-2006 The Regents of The University of Michigan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Steve Reinhardt
 *          Lisa Hsu
 *          Nathan Binkert
 *          Ali Saidi
 */

#include "arch/isa_traits.hh"
#include "base/loader/object_file.hh"
#include "base/loader/symtab.hh"
#include "base/trace.hh"
#include "cpu/thread_context.hh"
#include "mem/mem_object.hh"
#include "mem/physical.hh"
#include "sim/builder.hh"
#include "sim/byteswap.hh"
#include "sim/system.hh"
#if FULL_SYSTEM
#include "arch/vtophys.hh"
#include "base/remote_gdb.hh"
#include "kern/kernel_stats.hh"
#endif

using namespace std;
using namespace TheISA;

vector<System *> System::systemList;

int System::numSystemsRunning = 0;

System::System(Params *p)
    : SimObject(p->name), physmem(p->physmem), numcpus(0),
#if FULL_SYSTEM
      init_param(p->init_param),
      functionalPort(p->name + "-fport"),
      virtPort(p->name + "-vport"),
#else
      page_ptr(0),
#endif
      _params(p)
{
    // add self to global system list
    systemList.push_back(this);

#if FULL_SYSTEM
    kernelSymtab = new SymbolTable;
    debugSymbolTable = new SymbolTable;


    /**
     * Get a functional port to memory
     */
    Port *mem_port;
    mem_port = physmem->getPort("functional");
    functionalPort.setPeer(mem_port);
    mem_port->setPeer(&functionalPort);

    mem_port = physmem->getPort("functional");
    virtPort.setPeer(mem_port);
    mem_port->setPeer(&virtPort);


    /**
     * Load the kernel code into memory
     */
    // Load kernel code
    kernel = createObjectFile(params()->kernel_path);
    if (kernel == NULL)
        fatal("Could not load kernel file %s", params()->kernel_path);

    // Load program sections into memory
    kernel->loadSections(&functionalPort, LoadAddrMask);

    // setup entry points
    kernelStart = kernel->textBase();
    kernelEnd = kernel->bssBase() + kernel->bssSize();
    kernelEntry = kernel->entryPoint();

    // load symbols
    if (!kernel->loadGlobalSymbols(kernelSymtab))
        panic("could not load kernel symbols\n");

    if (!kernel->loadLocalSymbols(kernelSymtab))
        panic("could not load kernel local symbols\n");

    if (!kernel->loadGlobalSymbols(debugSymbolTable))
        panic("could not load kernel symbols\n");

    if (!kernel->loadLocalSymbols(debugSymbolTable))
        panic("could not load kernel local symbols\n");

    DPRINTF(Loader, "Kernel start = %#x\n", kernelStart);
    DPRINTF(Loader, "Kernel end   = %#x\n", kernelEnd);
    DPRINTF(Loader, "Kernel entry = %#x\n", kernelEntry);
    DPRINTF(Loader, "Kernel loaded...\n");

    kernelBinning = new Kernel::Binning(this);
#endif // FULL_SYSTEM

    // increment the number of running systms
    numSystemsRunning++;
}

System::~System()
{
#if FULL_SYSTEM
    delete kernelSymtab;
    delete kernel;
#else
    panic("System::fixFuncEventAddr needs to be rewritten "
          "to work with syscall emulation");
#endif // FULL_SYSTEM}
}

#if FULL_SYSTEM


int rgdb_wait = -1;

#endif // FULL_SYSTEM

int
System::registerThreadContext(ThreadContext *tc, int id)
{
    if (id == -1) {
        for (id = 0; id < threadContexts.size(); id++) {
            if (!threadContexts[id])
                break;
        }
    }

    if (threadContexts.size() <= id)
        threadContexts.resize(id + 1);

    if (threadContexts[id])
        panic("Cannot have two CPUs with the same id (%d)\n", id);

    threadContexts[id] = tc;
    numcpus++;

#if FULL_SYSTEM
    RemoteGDB *rgdb = new RemoteGDB(this, tc);
    GDBListener *gdbl = new GDBListener(rgdb, 7000 + id);
    gdbl->listen();
    /**
     * Uncommenting this line waits for a remote debugger to connect
     * to the simulator before continuing.
     */
    if (rgdb_wait != -1 && rgdb_wait == id)
        gdbl->accept();

    if (remoteGDB.size() <= id) {
        remoteGDB.resize(id + 1);
    }

    remoteGDB[id] = rgdb;
#endif // FULL_SYSTEM

    return id;
}

void
System::startup()
{
    int i;
    for (i = 0; i < threadContexts.size(); i++)
        threadContexts[i]->activate(0);
}

void
System::replaceThreadContext(ThreadContext *tc, int id)
{
    if (id >= threadContexts.size()) {
        panic("replaceThreadContext: bad id, %d >= %d\n",
              id, threadContexts.size());
    }

    threadContexts[id] = tc;
#if FULL_SYSTEM
    remoteGDB[id]->replaceThreadContext(tc);
#endif // FULL_SYSTEM
}

#if !FULL_SYSTEM
Addr
System::new_page()
{
    Addr return_addr = page_ptr << LogVMPageSize;
    ++page_ptr;
    return return_addr;
}
#endif

void
System::serialize(ostream &os)
{
#if FULL_SYSTEM
    kernelSymtab->serialize("kernel_symtab", os);
#endif // FULL_SYSTEM
}


void
System::unserialize(Checkpoint *cp, const string &section)
{
#if FULL_SYSTEM
    kernelSymtab->unserialize("kernel_symtab", cp, section);
#endif // FULL_SYSTEM
}

void
System::printSystems()
{
    vector<System *>::iterator i = systemList.begin();
    vector<System *>::iterator end = systemList.end();
    for (; i != end; ++i) {
        System *sys = *i;
        cerr << "System " << sys->name() << ": " << hex << sys << endl;
    }
}

extern "C"
void
printSystems()
{
    System::printSystems();
}

#if FULL_SYSTEM

// In full system mode, only derived classes (e.g. AlphaLinuxSystem)
// can be created directly.

DEFINE_SIM_OBJECT_CLASS_NAME("System", System)

#else

BEGIN_DECLARE_SIM_OBJECT_PARAMS(System)

    SimObjectParam<PhysicalMemory *> physmem;

END_DECLARE_SIM_OBJECT_PARAMS(System)

BEGIN_INIT_SIM_OBJECT_PARAMS(System)

    INIT_PARAM(physmem, "physical memory")

END_INIT_SIM_OBJECT_PARAMS(System)

CREATE_SIM_OBJECT(System)
{
    System::Params *p = new System::Params;
    p->name = getInstanceName();
    p->physmem = physmem;
    return new System(p);
}

REGISTER_SIM_OBJECT("System", System)

#endif
