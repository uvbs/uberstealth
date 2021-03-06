// Shared functionality between local and remote stealth sessions, respectively.
// The parameter to StealthSession contains all the debugger specific functionality.

#pragma once

#pragma warning(disable : 4201)
#include <WinIoCtl.h>
#pragma warning(default : 4201)
#include <iostream>
#include <boost/bind.hpp>
#include <boost/function.hpp>
#include <boost/make_shared.hpp>
#include <boost/shared_ptr.hpp>
#pragma warning(disable : 4244 4512)
#include <boost/thread.hpp>
#pragma warning(default : 4244 4512)
#include "DriverControl.h"
#include <HideDebugger/HideDebuggerProfile.h>
#include "IPCConfigExchangeWriter.h"
#include <common/InjectionBeacon.h>
#include <NInjectLib/IATModifier.h>
#include <NInjectLib/InjectLib.h>
#include <RDTSCEmu/driver/RDTSCEmu.h>
#include "resource.h"
#include "ResourceItem.h"
#include <StealthDriver/StealthDriver/StealthDriver.h>
#include "StealthSession.h"

namespace uberstealth {

template <typename LoggerT>
class CommonStealthSession : public StealthSession {
public:
	CommonStealthSession(const boost::filesystem::path& profilePath):
		hProcess_(INVALID_HANDLE_VALUE),
		profilePath_(profilePath),
		currentProfile_(HideDebuggerProfile::readProfileFromFile(profilePath)) {}
	virtual ~CommonStealthSession() {}
	
	virtual void handleDebuggerAttach(unsigned int processId) {
		if (currentProfile_.getEnableDbgAttachEnabled()) {
			// We need to start dll injection in a background thread because the dll injection
			// will block until the debuggee is resumed.
			boost::thread(boost::bind(&CommonStealthSession::dbgAttachThread, this, processId));
		}
	}

	virtual void handleDebuggerStart(unsigned int processId, uintptr_t baseAddress) {
		if (currentProfile_.getEnableDbgStartEnabled()) {
			// TODO(jan.newger@newgre.net): the exception should NOT be catched here; instead it should bubble up to the caller which then needs to handle it!
			try	{
				injectionBeacon_ = boost::make_shared<InjectionBeacon>(processId);
				performCommonInit(processId);
				handleRtlGetNtGlobalFlags(processId);

				Process process(processId);
				IATModifier iatMod(process);
				iatMod.setImageBase(baseAddress);
				ipc_ = boost::make_shared<uberstealth::IPCConfigExchangeWriter>(processId);
				ipc_->setProfileFile(profilePath_.string());
				ipc_->setIPCPEHeaderData(uberstealth::IPCPEHeaderData(baseAddress, iatMod.readNTHeaders()));
				ipc_->setPERestoreRequired(true);
				iatMod.writeIAT(getStealthDllPath());
			} catch (const std::exception& e) {
				logger_.logString("Failed to inject stealth dll (%s): %s.\n", getStealthDllPath().c_str(), e.what());
			} catch (...) {
				logger_.logString("Unknown error while trying to inject stealth dll (%s).\n", getStealthDllPath().c_str());
			}
		}
	}

	virtual void handleDebuggerExit() {
		stopDrivers();
		CloseHandle(hProcess_);
	}

protected:
	// TODO(jan.newger@newgre.net): should these methods be moved to LocalStealthSession? (these are useless in a RemoteStealth scenario)
	virtual ResourceItem getRDTSCDriverResource() =0;
	virtual ResourceItem getStealthDriverResource() =0;
	virtual std::string getStealthDllPath() =0;

	void handleRtlGetNtGlobalFlags(unsigned int processID) {
		if (!currentProfile_.getRtlGetNtGlobalFlagsEnabled()) {
			return;
		}

		// Assume that ntdll is loaded to the same IBA across processes on ASLR systems.
		HMODULE hNtDll = LoadLibrary(L"ntdll.dll");
		LPVOID address = GetProcAddress(hNtDll, "RtlGetNtGlobalFlags");
		if (address) {
			// xor eax, eax; retn
			unsigned char opcodes[] = { 0x31, 0xC0, 0xC3 };
			try	{
				Process process(processID);
				process.writeMemory(address, opcodes, 3);
			} catch (const MemoryAccessException& e){
				logger_.logString("Error while trying to patch RtlGetNtGlobalFlags: %s.\n", e.what());
			}
		}
		FreeLibrary(hNtDll);
	}

	void performCommonInit(unsigned int processID) {
		startDrivers();
		acquireProcessHandle(processID);
	}

	void startDrivers() {
		try {		
			if (currentProfile_.getRDTSCDriverLoad()) {
				ResourceItem drvResource = getRDTSCDriverResource();
				std::string rdtscName = currentProfile_.getRDTSCDriverCustomName();
				rdtscDriver_.startDriver(drvResource, rdtscName);
				RDTSCMode mode = currentProfile_.getRDTSCDriverMode();
				unsigned int param = mode == constant ? 0 : currentProfile_.getRDTSCDriverDelta();
				DWORD ioctlCode = mode == constant ? (DWORD)IOCTL_RDTSCEMU_METHOD_ALWAYS_CONST : (DWORD)IOCTL_RDTSCEMU_METHOD_INCREASING;
				rdtscDriver_.setMode(ioctlCode, &param, sizeof(param));
				logger_.logString("Successfully started RDTSC emulation driver from %s.\n", rdtscDriver_.getDriverPath().c_str());
			}
		} catch (const std::exception& e) {
			logger_.logString("Error while trying to load RDTSC driver: %s.\n", e.what());
		}

		try	{
			if (currentProfile_.getStealthDriverLoad()) {
				ResourceItem stealthDrvResource = getStealthDriverResource();
				std::string stealthName = currentProfile_.getStealthDriverCustomName();
				stealthDriver_.startDriver(stealthDrvResource, stealthName);
				logger_.logString("Successfully started stealth driver from %s.\n", stealthDriver_.getDriverPath().c_str());

				StealthHook mode;
				if (currentProfile_.getStealthDriverNtSetInformationThread()) {
					mode = SH_NtSetInformationThread;
					stealthDriver_.setMode(IOCTL_STEALTHDRIVER_ENABLE_HOOKS, &mode, sizeof(StealthHook));
				}
				if (currentProfile_.getStealthDriverNtQueryInformationProcess()) {
					mode = SH_NtQueryInformationProcess;
					stealthDriver_.setMode(IOCTL_STEALTHDRIVER_ENABLE_HOOKS, &mode, sizeof(StealthHook));
				}
			}
		}
		catch (const std::exception& e) {
			logger_.logString("Error while trying to load stealth driver: %s.\n", e.what());
		}
	}

	void stopDrivers() {
		if (currentProfile_.getRDTSCDriverUnload()) {
			try {
				bool wasRunning = rdtscDriver_.isRunning();
				rdtscDriver_.stopDriver();
				if (wasRunning) logger_.logString("Successfully unloaded RDTSC emulation driver");
			} catch (const std::exception& e) {
				logger_.logString("Error while trying to stop RDTSC driver: %s\n", e.what());
			}
		}

		if (currentProfile_.getStealthDriverUnload()) {
			try	{
				bool wasRunning = stealthDriver_.isRunning();
				stealthDriver_.stopDriver();
				if (wasRunning) logger_.logString("Successfully unloaded stealth driver.\n");
			} catch (const std::exception& e) {
				logger_.logString("Error while trying to stop stealth driver: %s.\n", e.what());
			}
		}
	}

	HANDLE getProcessHandle() const { return hProcess_; };
	LoggerT logger_;
	HideDebuggerProfile currentProfile_;

private:
	void dbgAttachThread(unsigned int processId) {
		try	{
			injectionBeacon_ = boost::make_shared<InjectionBeacon>(processId);
			performCommonInit(processId);
			ipc_ = boost::make_shared<uberstealth::IPCConfigExchangeWriter>(processId);
			ipc_->setProfileFile(profilePath_.string());
			ipc_->setPERestoreRequired(false);

			Process process(processId);
			InjectLibrary injector(getStealthDllPath(), process);
			if (!injector.injectLib()) {
				logger_.logString("Injection of stealth dll failed (while attaching to process).\n");
			}
		} catch (const std::exception& e) {
			logger_.logString("Error while trying to attach to process: %s\n", e.what());
		}
	}

	void acquireProcessHandle(unsigned int processID) {
		hProcess_ = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processID);
		if (hProcess_ == INVALID_HANDLE_VALUE) {
			throw std::runtime_error("Unable to obtain process handle from debuggee.");
		}
	}

	boost::shared_ptr<uberstealth::IPCConfigExchangeWriter> ipc_;
	DriverControl rdtscDriver_;
	DriverControl stealthDriver_;
	HANDLE hProcess_;
	boost::shared_ptr<InjectionBeacon> injectionBeacon_;
	boost::filesystem::path profilePath_;
};

}