// Copyright 2001-2018 Crytek GmbH / Crytek Group. All rights reserved.

/*=============================================================================
   RenderThread.cpp: Render thread commands processing.

   Revision history:
* Created by Honich Andrey

   =============================================================================*/

#include "StdAfx.h"

#include <CrySystem/Scaleform/IFlashPlayer.h>
#include <Cry3DEngine/I3DEngine.h>
#include "RenderAuxGeom.h"
#include "IColorGradingControllerInt.h"
#include <CrySystem/Profilers/IStatoscope.h>
#include <CryGame/IGameFramework.h>
#include <CryAnimation/ICryAnimation.h>
#include "PostProcess/PostEffects.h"
#include <CryThreading/IThreadManager.h>

#include <DriverD3D.h>

#include <cstring>

#include "RenderView.h"

#ifdef STRIP_RENDER_THREAD
	#define m_nCurThreadFill    0
	#define m_nCurThreadProcess 0
#endif

#undef MULTITHREADED_RESOURCE_CREATION

// Only needed together with render thread.
struct SRenderThreadLocalStorage
{
	int currentCommandBuffer;

	SRenderThreadLocalStorage() : currentCommandBuffer(0) {};
};


#if CRY_PLATFORM_WINDOWS
HWND SRenderThread::GetRenderWindowHandle()
{
	return (HWND)gRenDev->GetCurrentContextHWND();
}
#endif

void CRenderThread::ThreadEntry()
{

	threadID renderThreadId = CryGetCurrentThreadId();
	gRenDev->m_pRT->m_nRenderThread = renderThreadId;
	CNameTableR::m_nRenderThread = renderThreadId;
	gEnv->pCryPak->SetRenderThreadId(renderThreadId);
	m_started.Set();
	gRenDev->m_pRT->Process();
}

void CRenderThreadLoading::ThreadEntry()
{
	threadID renderLoadingThreadId = CryGetCurrentThreadId();
	gRenDev->m_pRT->m_nRenderThreadLoading = renderLoadingThreadId;
	CNameTableR::m_nRenderThread = renderLoadingThreadId;

	// We aren't interested in file access from the render loading thread, and this
	// would overwrite the real render thread id
	// gEnv->pCryPak->SetRenderThreadId( renderThreadId );
	m_started.Set();
	gRenDev->m_pRT->ProcessLoading();
}

void SRenderThread::SwitchMode(bool bEnableVideo)
{
	if (bEnableVideo)
	{
		assert(IsRenderThread());
		if (m_pThreadLoading)
			return;
#if !defined(STRIP_RENDER_THREAD)
		SSystemGlobalEnvironment* pEnv = iSystem->GetGlobalEnvironment();
		if (pEnv && !pEnv->bTesting && !pEnv->IsEditor() && pEnv->pi.numCoresAvailableToProcess > 1 && CRenderer::CV_r_multithreaded > 0)
		{
			m_pThreadLoading = new CRenderThreadLoading();
		}
		m_eVideoThreadMode = eVTM_Active;
		m_bQuitLoading = false;
		StartRenderLoadingThread();
#endif
	}
	else
	{
		m_eVideoThreadMode = eVTM_ProcessingStop;
	}
}

SRenderThread::SRenderThread()
{
	m_eVideoThreadMode = eVTM_Disabled;
	m_nRenderThreadLoading = 0;
	m_nLevelLoadingThread = 0;
	m_pThreadLoading = 0;
	m_pLoadtimeCallback = 0;
	m_bEndFrameCalled = false;
	m_bBeginFrameCalled = false;
	m_bQuitLoading = false;
#if CRY_PLATFORM_DURANGO
	m_suspendWhileLoadingFlag = 0;
#endif
	Init();
}

threadID CNameTableR::m_nRenderThread = 0;

void SRenderThread::Init()
{
	m_bQuit = false;
#ifndef STRIP_RENDER_THREAD
	m_nCurThreadFill = 0;
	m_nCurThreadProcess = 0;
#endif
	InitFlushCond();

	m_nRenderThread = ::GetCurrentThreadId();
	CNameTableR::m_nRenderThread = m_nRenderThread;
	m_nMainThread = m_nRenderThread;
	m_bSuccessful = true;
	m_pThread = NULL;
#if !defined(STRIP_RENDER_THREAD)
	SSystemGlobalEnvironment* pEnv = iSystem->GetGlobalEnvironment();
	//if (pEnv && !pEnv->bTesting && !pEnv->IsDedicated() && !pEnv->IsEditor() && pEnv->pi.numCoresAvailableToProcess > 1 && CRenderer::CV_r_multithreaded > 0)
	if (pEnv && !pEnv->bTesting && !pEnv->IsDedicated() && pEnv->pi.numCoresAvailableToProcess > 1 && CRenderer::CV_r_multithreaded > 0)
	{
		m_nCurThreadProcess = 1;
		m_pThread = new CRenderThread();
	}
	#ifndef CONSOLE_CONST_CVAR_MODE
	else
		CRenderer::CV_r_multithreaded = 0;
	#endif
#else//STRIP_RENDER_THREAD
	#ifndef CONSOLE_CONST_CVAR_MODE
	CRenderer::CV_r_multithreaded = 0;
	#endif
#endif//STRIP_RENDER_THREAD
	gRenDev->m_nProcessThreadID = threadID(m_nCurThreadProcess);
	gRenDev->m_nFillThreadID = threadID(m_nCurThreadFill);

	for (uint32 i = 0; i < RT_COMMAND_BUF_COUNT; ++i)
	{
		m_Commands[i].Free();
		m_Commands[i].Create(300 * 1024); // 300 to stop growing in MP levels
		m_Commands[i].SetUse(0);
	}

	m_eVideoThreadMode = eVTM_Disabled;
}

SRenderThread::~SRenderThread()
{
	QuitRenderLoadingThread();
	QuitRenderThread();
}

//==============================================================================================
// NOTE: Render commands can be added from main thread only

bool SRenderThread::RC_CreateDevice()
{
	LOADING_TIME_PROFILE_SECTION;

#if CRY_PLATFORM_WINDOWS || CRY_PLATFORM_APPLE || CRY_PLATFORM_LINUX || CRY_PLATFORM_ANDROID
	return gcpRendD3D->RT_CreateDevice();
#else
	if (IsRenderThread())
	{
		return gcpRendD3D->RT_CreateDevice();
	}
	byte* p = AddCommand(eRC_CreateDevice, 0);
	EndCommand(p);

	FlushAndWait();

	return !IsFailed();
#endif
}

void SRenderThread::RC_ResetDevice()
{
#if CRY_PLATFORM_WINDOWS || CRY_PLATFORM_LINUX || CRY_PLATFORM_ANDROID || CRY_PLATFORM_APPLE
	gcpRendD3D->RT_Reset();
#else
	if (IsRenderThread())
	{
		gcpRendD3D->RT_Reset();
		return;
	}
	byte* p = AddCommand(eRC_ResetDevice, 0);
	EndCommand(p);

	FlushAndWait();
#endif
}

#if CRY_PLATFORM_DURANGO
void SRenderThread::RC_SuspendDevice()
{
	if (m_eVideoThreadMode != eVTM_Disabled)
	{
		if (!gRenDev->m_bDeviceSuspended)
		{
			m_suspendWhileLoadingEvent.Reset();
			m_suspendWhileLoadingFlag = 1;

			do
			{
				CryLowLatencySleep(1);
			}
			while (m_suspendWhileLoadingFlag != 0);
		}
		return;
	}

	if (IsRenderThread())
	{
		return gcpRendD3D->RT_SuspendDevice();
	}
	byte* p = AddCommand(eRC_SuspendDevice, 0);
	EndCommand(p);

	FlushAndWait();
}

void SRenderThread::RC_ResumeDevice()
{
	if (m_eVideoThreadMode != eVTM_Disabled)
	{
		if (gRenDev->m_bDeviceSuspended)
		{
			//here we've got render thread waiting in event
			//wake em up
			m_suspendWhileLoadingEvent.Set();
		}
		return;
	}

	if (IsRenderThread())
	{
		return gcpRendD3D->RT_ResumeDevice();
	}
	byte* p = AddCommand(eRC_ResumeDevice, 0);
	EndCommand(p);

	FlushAndWait();
}
#endif

void SRenderThread::RC_BeginFrame(const SDisplayContextKey& displayContextKey)
{
	if (IsRenderThread())
	{
		// NOTE: bypasses time measurement!
		gcpRendD3D->RT_BeginFrame(displayContextKey);
		return;
	}

	byte* p = AddCommand(eRC_BeginFrame, sizeof(displayContextKey));
	if (sizeof(displayContextKey) == 8)
		AddQWORD(p, *reinterpret_cast<const uint64*>(&displayContextKey));
	else if (sizeof(displayContextKey) == 16)
	{
		AddQWORD(p, *reinterpret_cast<const uint64*>(&displayContextKey));
		AddQWORD(p, *(reinterpret_cast<const uint64*>(&displayContextKey) + 1));
	}
	else
		__debugbreak();
	EndCommand(p);
}

void SRenderThread::RC_EndFrame(bool bWait)
{
	if (IsRenderThread())
	{
		// NOTE: bypasses time measurement!
		gcpRendD3D->RT_EndFrame();
		SyncMainWithRender(true);
		return;
	}
	if (!bWait && CheckFlushCond())
		return;

	if (m_eVideoThreadMode == eVTM_Disabled)
	{
		AUTO_LOCK_T(CryCriticalSectionNonRecursive, m_CommandsLoadingLock); 

		if (const unsigned int size = m_CommandsLoading.size())
		{
			byte* buf = m_Commands[m_nCurThreadFill].Grow(size);
			memcpy(buf, &m_CommandsLoading[0], size);
			m_CommandsLoading.Free();
		}
	}

	byte* p = AddCommand(eRC_EndFrame, 0);
	EndCommand(p);
	SyncMainWithRender(true);
}

void SRenderThread::RC_PrecacheResource(ITexture* pTP, float fMipFactor, const CTimeValue& fTimeToReady, int Flags, int nUpdateId, int nCounter)
{
	if (!pTP)
		return;

	if (IsRenderThread())
	{
		// NOTE: bypasses time measurement!
		gRenDev->PrecacheTexture(pTP, fMipFactor, fTimeToReady, Flags, nUpdateId, nCounter);
		return;
	}

	_smart_ptr<ITexture> pRefTexture = pTP;
	ExecuteRenderThreadCommand([=]
	{
		RC_PrecacheResource(pRefTexture.get(), fMipFactor, fTimeToReady, Flags, nUpdateId, nCounter);
	}, ERenderCommandFlags::LevelLoadingThread_defer);
}

void SRenderThread::RC_TryFlush()
{
	if (IsRenderThread())
	{
		return;
	}

	// do nothing if the render thread is still busy
	if (CheckFlushCond())
		return;

	gRenDev->GetIRenderAuxGeom()->Submit(); // need to be submitted in main thread's aux cb before EndFrame (otherwise it is processed after p3dDev->EndScene())
	
	FlushAndWait();
}

void SRenderThread::RC_FlashRenderPlayer(std::shared_ptr<IFlashPlayer> &&pPlayer)
{
	assert(IsRenderThread());
	gcpRendD3D->RT_FlashRenderInternal(std::move(pPlayer));
}

void SRenderThread::RC_FlashRender(std::shared_ptr<IFlashPlayer_RenderProxy> &&pPlayer)
{
	if (IsRenderThread())
	{
		// NOTE: bypasses time measurement!
		gcpRendD3D->RT_FlashRenderInternal(std::move(pPlayer), true);
		return;
	}

	byte* p = AddCommand(eRC_FlashRender, sizeof(std::shared_ptr<IFlashPlayer_RenderProxy>));

	// Write the shared_ptr without releasing a reference.
	StoreUnaligned<std::shared_ptr<IFlashPlayer_RenderProxy>>(p, pPlayer);
	p += sizeof(std::shared_ptr<IFlashPlayer_RenderProxy>);
	std::memset(&pPlayer, 0, sizeof(std::shared_ptr<IFlashPlayer_RenderProxy>));

	EndCommand(p);
}

void SRenderThread::RC_FlashRenderPlaybackLockless(std::shared_ptr<IFlashPlayer_RenderProxy> &&pPlayer, int cbIdx, bool finalPlayback)
{
	if (IsRenderThread())
	{
		// NOTE: bypasses time measurement!
		gcpRendD3D->RT_FlashRenderPlaybackLocklessInternal(std::move(pPlayer), cbIdx, finalPlayback, true);
		return;
	}

	byte* p = AddCommand(eRC_FlashRenderLockless, 12 + sizeof(std::shared_ptr<IFlashPlayer_RenderProxy>));

	// Write the shared_ptr without releasing a reference.
	StoreUnaligned<std::shared_ptr<IFlashPlayer_RenderProxy>>(p, pPlayer);
	p += sizeof(std::shared_ptr<IFlashPlayer_RenderProxy>);
	std::memset(&pPlayer, 0, sizeof(std::shared_ptr<IFlashPlayer_RenderProxy>));

	AddDWORD(p, (uint32) cbIdx);
	AddDWORD(p, finalPlayback ? 1 : 0);
	EndCommand(p);
}

void SRenderThread::RC_StartVideoThread()
{
	byte* p = AddCommandTo(eRC_LambdaCall, sizeof(void*), m_Commands[m_nCurThreadFill]);
	void* pCallbackPtr = ::new(m_lambdaCallbacksPool.Allocate()) SRenderThreadLambdaCallback{ [=] { this->m_eVideoThreadMode = eVTM_RequestStart; } , ERenderCommandFlags::None };
	AddPointer(p, pCallbackPtr);
	EndCommandTo(p, m_Commands[m_nCurThreadFill]);
}

void SRenderThread::RC_StopVideoThread()
{
	byte* p = AddCommandTo(eRC_LambdaCall, sizeof(void*), m_Commands[m_nCurThreadFill]);
	void* pCallbackPtr = ::new(m_lambdaCallbacksPool.Allocate()) SRenderThreadLambdaCallback{ [=] { this->m_eVideoThreadMode = eVTM_RequestStop; } , ERenderCommandFlags::None };
	AddPointer(p, pCallbackPtr);
	EndCommandTo(p, m_Commands[m_nCurThreadFill]);
}

//===========================================================================================

#ifdef DO_RENDERSTATS
	#define START_PROFILE_RT_SCOPE() CTimeValue Time = GTimer(d3d)->GetAsyncTime();
	#define START_PROFILE_RT() Time = GTimer(d3d)->GetAsyncTime();
	#define END_PROFILE_PLUS_RT(Dst) Dst += GTimer(d3d)->GetAsyncTime() - Time;
	#define END_PROFILE_RT(Dst)      Dst = GTimer(d3d)->GetAsyncTime() - Time;
#else
	#define START_PROFILE_RT_SCOPE()
	#define START_PROFILE_RT()
	#define END_PROFILE_PLUS_RT(Dst)
	#define END_PROFILE_RT(Dst)
#endif

#pragma warning(push)
#pragma warning(disable : 4800)
void SRenderThread::ProcessCommands()
{
#ifndef STRIP_RENDER_THREAD
	assert(IsRenderThread());
	if (!CheckFlushCond())
		return;

	DWORD nDeviceOwningThreadID = gcpRendD3D->GetBoundThreadID();
	if (m_eVideoThreadMode == eVTM_Disabled)
		gcpRendD3D->BindContextToThread(CryGetCurrentThreadId());

	#if CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION
		#if OGL_SINGLE_CONTEXT
	if (m_eVideoThreadMode == eVTM_Disabled)
		m_kDXGLDeviceContextHandle.Set(gcpRendD3D->GetDeviceContext().GetRealDeviceContext());
		#else
	if (CRenderer::CV_r_multithreaded)
		m_kDXGLContextHandle.Set(gcpRendD3D->GetDevice().GetRealDevice());
	if (m_eVideoThreadMode == eVTM_Disabled)
		m_kDXGLDeviceContextHandle.Set(gcpRendD3D->GetDeviceContext().GetRealDeviceContext(), !CRenderer::CV_r_multithreaded);
		#endif
	#endif //CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION

#if DO_RENDERSTATS
	CTimeValue Time;
#endif

	int threadId = m_nCurThreadProcess;

	#if CRY_PLATFORM_DURANGO
	bool bSuspendDevice = false;
	#endif
	int n = 0;
	m_bSuccessful = true;
	m_hResult = S_OK;
	byte* pP;
	while (n < (int)m_Commands[threadId].Num())
	{
		pP = &m_Commands[threadId][n];
		n += sizeof(int);
		byte nC = (byte) * ((int*)pP);

	#if !defined(_RELEASE)
		// Ensure that the command hasn't been processed already
		int* pProcessed = (int*)(pP + sizeof(int));
		IF_UNLIKELY (*pProcessed)
			__debugbreak();
		*pProcessed = 1;
		n += sizeof(int);
	#endif

		switch (nC)
		{
		case eRC_CreateDevice:
		{
			CRY_PROFILE_REGION(PROFILE_RENDERER, "SRenderThread: eRC_CreateDevice");
			START_PROFILE_RT();
			m_bSuccessful &= gcpRendD3D->RT_CreateDevice();
			END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.miscTime);
		}
			break;
		case eRC_ResetDevice:
		{
			CRY_PROFILE_REGION(PROFILE_RENDERER, "SRenderThread: eRC_ResetDevice");
			START_PROFILE_RT();
			if (m_eVideoThreadMode == eVTM_Disabled)
				gcpRendD3D->RT_Reset();
			END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.miscTime);
		}
			break;
	#if CRY_PLATFORM_DURANGO
		case eRC_SuspendDevice:
		{
			CRY_PROFILE_REGION(PROFILE_RENDERER, "SRenderThread: eRC_SuspendDevice");
			START_PROFILE_RT();
			if (m_eVideoThreadMode == eVTM_Disabled)
				gcpRendD3D->RT_SuspendDevice();
			END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.miscTime);
		}
			break;
		case eRC_ResumeDevice:
		{
			CRY_PROFILE_REGION(PROFILE_RENDERER, "SRenderThread: eRC_ResumeDevice");
			START_PROFILE_RT();
			if (m_eVideoThreadMode == eVTM_Disabled)
			{
				gcpRendD3D->RT_ResumeDevice();
				//Now we really want to resume the device
				bSuspendDevice = false;
		}
			END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.miscTime);
		}
			break;
	#endif

		case eRC_BeginFrame:
		{
			CRY_PROFILE_REGION(PROFILE_RENDERER, "SRenderThread: eRC_BeginFrame");
			START_PROFILE_RT();
			m_displayContextKey = ReadCommand<SDisplayContextKey>(n);
			if (m_eVideoThreadMode == eVTM_Disabled)
			{
				gcpRendD3D->RT_BeginFrame(m_displayContextKey);
				m_bBeginFrameCalled = false;
			}
			else
			{
				m_bBeginFrameCalled = true;
			}
			END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.renderTime);
		}
			break;
		case eRC_EndFrame:
		{
			CRY_PROFILE_REGION(PROFILE_RENDERER, "SRenderThread: eRC_EndFrame");
			START_PROFILE_RT();
			if (m_eVideoThreadMode == eVTM_Disabled)
			{
				gcpRendD3D->RT_EndFrame();
				m_bEndFrameCalled = false;
			}
			else
			{
				// RLT handles precache commands - so all texture streaming prioritisation
				// needs to happen here. Scheduling and device texture management will happen
				// on the RT later.
				CTexture::RLT_LoadingUpdate();

				m_bEndFrameCalled = true;
				gcpRendD3D->m_nFrameSwapID++;
			}
			END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.renderTime);
		}
			break;

		case eRC_FlashRender:
			{
				START_PROFILE_RT();
				std::shared_ptr<IFlashPlayer_RenderProxy> pPlayer = ReadCommand<std::shared_ptr<IFlashPlayer_RenderProxy>>(n);
				gcpRendD3D->RT_FlashRenderInternal(std::move(pPlayer), m_eVideoThreadMode == eVTM_Disabled);
				END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.flashTime);
			}
			break;
		case eRC_FlashRenderLockless:
			{
				START_PROFILE_RT();
				std::shared_ptr<IFlashPlayer_RenderProxy> pPlayer = ReadCommand<std::shared_ptr<IFlashPlayer_RenderProxy>>(n);
				int cbIdx = ReadCommand<int>(n);
				bool finalPlayback = ReadCommand<int>(n) != 0;
				gcpRendD3D->RT_FlashRenderPlaybackLocklessInternal(std::move(pPlayer), cbIdx, finalPlayback, m_eVideoThreadMode == eVTM_Disabled);
				END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.flashTime);
		}
			break;

		case eRC_LambdaCall:
		{
			CRY_PROFILE_REGION(PROFILE_RENDERER, "SRenderThread: eRC_LambdaCall");
			START_PROFILE_RT();
			SRenderThreadLambdaCallback* pRTCallback = ReadCommand<SRenderThreadLambdaCallback*>(n);
			bool bSkipCommand = (m_eVideoThreadMode != eVTM_Disabled) && (uint32(pRTCallback->flags & ERenderCommandFlags::SkipDuringLoading) != 0);
			// Execute lambda callback on a render thread
			if (!bSkipCommand)
			{
				pRTCallback->callback();
			}

			m_lambdaCallbacksPool.Delete(pRTCallback);
			END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.renderTime);
		}
			break;

		default:
			{
				assert(0);
			}
			break;
		}
	}

	if (m_eVideoThreadMode == eVTM_Disabled)
		gcpRendD3D->BindContextToThread(nDeviceOwningThreadID);
#endif //STRIP_RENDER_THREAD
}
#pragma warning(pop)

void SRenderThread::Process()
{
	while (true)
	{
		CRY_PROFILE_REGION(PROFILE_RENDERER, "Loop: RenderThread");

		CTimeValue Time = GTimer(d3d)->GetAsyncTime();

		WaitFlushCond();
		const uint64 start = CryGetTicks();

		if (m_bQuit)
		{
			SignalFlushFinishedCond();
			break;//put it here to safely shut down
		}

		if (m_eVideoThreadMode == eVTM_Disabled)
		{
			CTimeValue TimeAfterWait = GTimer(d3d)->GetAsyncTime();
			if (gRenDev->m_bStartLevelLoading)
				SRenderStatistics::Write().m_Summary.idleLoading += (TimeAfterWait - Time);

			ProcessCommands();
			SignalFlushFinishedCond();

			CTimeValue fT = GTimer(d3d)->GetAsyncTime() - TimeAfterWait;
			if (gRenDev->m_bStartLevelLoading)
				SRenderStatistics::Write().m_Summary.busyLoading += fT;
		}

		if (m_eVideoThreadMode == eVTM_RequestStart)
		{
			uint32 frameId = gRenDev->GetRenderFrameID();
			DWORD nDeviceOwningThreadID = gcpRendD3D->GetBoundThreadID();
			gcpRendD3D->BindContextToThread(CryGetCurrentThreadId());
			gRenDev->m_DevBufMan.Sync(frameId); // make sure no request are flying when switching to render loading thread
#if defined(ENABLE_SIMPLE_GPU_TIMERS)
			gcpRendD3D->m_pPipelineProfiler->SetPaused(true);
#endif
			// Create another render thread;
			SwitchMode(true);

			{
				CTimeValue lastTime = GetGTimer()->GetAsyncTime();

				while (m_eVideoThreadMode != eVTM_ProcessingStop)
				{
#if CRY_PLATFORM_DURANGO
					if (m_suspendWhileLoadingFlag)
					{
						threadID nLoadingThreadId = gEnv->pThreadManager->GetThreadId(RENDER_LOADING_THREAD_NAME);
						HANDLE hHandle = OpenThread(THREAD_ALL_ACCESS, TRUE, nLoadingThreadId);
						DWORD result = SuspendThread(hHandle);

						CryLogAlways("SuspendWhileLoading: Suspend result = %d", result);
						gcpRendD3D->RT_SuspendDevice();

						m_suspendWhileLoadingFlag = 0;     //notify main thread, so suspending deferral can be completed
						m_suspendWhileLoadingEvent.Wait(); //wait until 'resume' will be received

						gcpRendD3D->RT_ResumeDevice();

						result = ResumeThread(hHandle);
						CryLogAlways("SuspendWhileLoading: Resume result = %d", result);
						CloseHandle(hHandle);
					}
#endif

					frameId += 1;
					CTimeValue curTime = GetGTimer()->GetAsyncTime();
					CTimeValue deltaTime = max(curTime - lastTime, CTimeValue(0));
					lastTime = curTime;
					gRenDev->m_DevBufMan.Update(frameId, true);

					if (m_pLoadtimeCallback)
						m_pLoadtimeCallback->LoadtimeUpdate(deltaTime);

					{
						m_rdldLock.Lock();

						if (m_pLoadtimeCallback)
							m_pLoadtimeCallback->LoadtimeRender();

						gRenDev->m_DevBufMan.ReleaseEmptyBanks(frameId);

						gcpRendD3D->RT_PresentFast();
						CRenderMesh::Tick();
						CTexture::RT_LoadingUpdate();
						m_rdldLock.Unlock();
					}

					// Make sure we aren't running with thousands of FPS with VSync disabled
					gRenDev->LimitFramerate(120);

#if defined(SUPPORT_DEVICE_INFO_MSG_PROCESSING)
					gcpRendD3D->DevInfo().ProcessSystemEventQueue();
#endif
				}
			}
			if (m_pThreadLoading)
				QuitRenderLoadingThread();
			m_eVideoThreadMode = eVTM_Disabled;

			// NOTE: bypasses time measurement!
			if (m_bBeginFrameCalled)
			{
				m_bBeginFrameCalled = false;
				gcpRendD3D->RT_BeginFrame(m_displayContextKey);
			}
			if (m_bEndFrameCalled)
			{
				m_bEndFrameCalled = false;
				gcpRendD3D->RT_EndFrame();
			}

#if defined(ENABLE_SIMPLE_GPU_TIMERS)
			gcpRendD3D->m_pPipelineProfiler->SetPaused(false);
#endif
			gcpRendD3D->BindContextToThread(nDeviceOwningThreadID);
		}

		const uint64 elapsed = CryGetTicks() - start;
		gEnv->pSystem->GetCurrentUpdateTimeStats().RenderTime = elapsed;
	}
#if CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION
	#if OGL_SINGLE_CONTEXT
	m_kDXGLDeviceContextHandle.Set(NULL);
	#else
	m_kDXGLDeviceContextHandle.Set(NULL, !CRenderer::CV_r_multithreaded);
	m_kDXGLContextHandle.Set(NULL);
	#endif
#endif //CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION
}

void SRenderThread::ProcessLoading()
{
	while (true)
	{
		CTimeValue fTime = GTimer(d3d)->GetAsyncCurTime();

		WaitFlushCond();
		if (m_bQuitLoading)
		{
			SignalFlushFinishedCond();
			break;//put it here to safely shut down
		}
		
		{
			CTimeValue fTimeAfterWait = GTimer(d3d)->GetAsyncCurTime();
			if (gRenDev->m_bStartLevelLoading)
				SRenderStatistics::Write().m_Summary.idleLoading += (fTimeAfterWait - fTime);

			// NOTE:
			ProcessCommands();
			SignalFlushFinishedCond();

			CTimeValue fTimeAfterProcess = GTimer(d3d)->GetAsyncCurTime();
			if (gRenDev->m_bStartLevelLoading)
				SRenderStatistics::Write().m_Summary.busyLoading += (fTimeAfterProcess - fTimeAfterWait);
		}

		if (m_eVideoThreadMode == eVTM_RequestStop)
		{
			// Switch to general render thread
			SwitchMode(false);
		}
	}
#if CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION
	#if OGL_SINGLE_CONTEXT
	m_kDXGLDeviceContextHandle.Set(NULL);
	#else
	m_kDXGLDeviceContextHandle.Set(NULL, !CRenderer::CV_r_multithreaded);
	m_kDXGLContextHandle.Set(NULL);
	#endif
#endif //CRY_RENDERER_OPENGL && !DXGL_FULL_EMULATION
}

#ifndef STRIP_RENDER_THREAD
// Flush current frame and wait for result (main thread only)
void SRenderThread::FlushAndWait()
{
	if (IsRenderThread())
		return;

	CRY_PROFILE_FUNCTION(PROFILE_RENDERER);

	if (gEnv->pStatoscope)
		gEnv->pStatoscope->LogCallstack("Flush Render Thread");

	if (!m_pThread)
		return;

	// NOTE: Execute twice to make the Process-/Fill-ThreadID toggle invisible too the outside
	SyncMainWithRender();
	SyncMainWithRender();
}
#endif//STRIP_RENDER_THREAD

// Flush current frame without waiting (should be called from main thread)
void SRenderThread::SyncMainWithRender(bool bFrameToFrame)
{
	CRY_PROFILE_REGION_WAITING(PROFILE_RENDERER, "Wait - SyncMainWithRender");
	CRYPROFILE_SCOPE_PROFILE_MARKER("SyncMainWithRender");

	if (!IsMultithreaded())
	{
		gRenDev->SyncMainWithRender();
		return;
	}

#ifndef STRIP_RENDER_THREAD
	WaitFlushFinishedCond();

	{
		START_PROFILE_RT_SCOPE();
		CPostEffectsMgr* pPostEffectMgr = PostEffectMgr();
		if (pPostEffectMgr)
		{
			// Must be called before the thread ID's get swapped
			pPostEffectMgr->SyncMainWithRender();
		}

		gRenDev->SyncMainWithRender();
		END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.miscTime);
	}

	// Register all times of this frame (including from two lines above)
	if (bFrameToFrame)
	{
		gcpRendD3D->RT_EndMeasurement();
	}

	//	gRenDev->ToggleMainThreadAuxGeomCB();
	gRenDev->m_nRenderThreadFrameID = gRenDev->GetMainFrameID();

	m_nCurThreadProcess = m_nCurThreadFill;
	m_nCurThreadFill    = (m_nCurThreadProcess + 1) & 1;
	gRenDev->m_nProcessThreadID = threadID(m_nCurThreadProcess);
	gRenDev->m_nFillThreadID    = threadID(m_nCurThreadFill);
	m_Commands[m_nCurThreadFill].SetUse(0);

	// Open a new timing-scope after all times have been registered (see RT_EndMeasurement)
	if (bFrameToFrame)
	{
		SRenderStatistics::s_pCurrentOutput  = &gRenDev->m_frameRenderStats[m_nCurThreadProcess];
		SRenderStatistics::s_pPreviousOutput = &gRenDev->m_frameRenderStats[m_nCurThreadFill   ];
		SRenderStatistics::s_pCurrentOutput->Begin(SRenderStatistics::s_pPreviousOutput);
	}
	//gRenDev->m_RP.m_pCurrentRenderView->PrepareForRendering();

	if (gEnv->pCharacterManager)
	{
		gEnv->pCharacterManager->UpdateRendererFrame();
	}

	SignalFlushCond();
#endif
}

void SRenderThread::QuitRenderThread()
{
	if (IsMultithreaded() && m_pThread)
	{
		SignalQuitCond();

#if defined(USE_LOCKS_FOR_FLUSH_SYNC)
		while (!gEnv->pThreadManager->JoinThread(m_pThread, eJM_TryJoin))
		{
			FlushAndWait();
		}
#else
		gEnv->pThreadManager->JoinThread(m_pThread, eJM_Join);
#endif

		SAFE_DELETE(m_pThread);

#if !defined(STRIP_RENDER_THREAD)
		m_nCurThreadProcess = m_nCurThreadFill;
#endif
	}
	m_bQuit = 1;
	//SAFE_RELEASE(m_pFlashPlayer);
}

void SRenderThread::QuitRenderLoadingThread()
{
	if (IsMultithreaded() && m_pThreadLoading)
	{
		FlushAndWait();
		m_bQuitLoading = true;
		gEnv->pThreadManager->JoinThread(m_pThreadLoading, eJM_Join);
		SAFE_DELETE(m_pThreadLoading);
		m_nRenderThreadLoading = 0;
		CNameTableR::m_nRenderThread = m_nRenderThread;
	}
}

void SRenderThread::StartRenderThread()
{
	if (m_pThread != NULL)
	{
		if (!gEnv->pThreadManager->SpawnThread(m_pThread, RENDER_THREAD_NAME))
		{
			CryFatalError("Error spawning \"%s\" thread.", RENDER_THREAD_NAME);
		}

		m_pThread->m_started.Wait();
	}
}

void SRenderThread::StartRenderLoadingThread()
{
	if (m_pThreadLoading != NULL)
	{
		if (!gEnv->pThreadManager->SpawnThread(m_pThreadLoading, RENDER_LOADING_THREAD_NAME))
		{
			CryFatalError("Error spawning \"%s\" thread.", RENDER_LOADING_THREAD_NAME);
		}

		m_pThreadLoading->m_started.Wait();
	}
}

bool SRenderThread::IsFailed()
{
	return !m_bSuccessful;
}

bool CRenderer::FlushRTCommands(bool bWait, bool bImmediatelly, bool bForce)
{
	SRenderThread* pRT = m_pRT;
	IF (!pRT, 0)
		return true;
	if (pRT->IsRenderThread(true))
	{
		SSystemGlobalEnvironment* pEnv = iSystem->GetGlobalEnvironment();
		if (pEnv && pEnv->IsEditor())
		{
			CPostEffectsMgr* pPostEffectMgr = PostEffectMgr();
			if (pPostEffectMgr)
			{
				pPostEffectMgr->SyncMainWithRender();
			}
		}
		return true;
	}
	if (!bForce && (!m_bStartLevelLoading || !pRT->IsMultithreaded()))
		return false;
	if (!bImmediatelly && pRT->CheckFlushCond())
		return false;
	if (bWait)
		pRT->FlushAndWait();

	return true;
}

bool CRenderer::ForceFlushRTCommands()
{
	LOADING_TIME_PROFILE_SECTION;
	return FlushRTCommands(true, true, true);
}

// Must be executed from main thread
void SRenderThread::WaitFlushFinishedCond()
{
	CRY_PROFILE_FUNCTION(PROFILE_RENDERER);
	START_PROFILE_RT_SCOPE();

#ifdef USE_LOCKS_FOR_FLUSH_SYNC
	m_LockFlushNotify.Lock();
	while (*(volatile int*)&m_nFlush)
	{
		m_FlushFinishedCondition.Wait(m_LockFlushNotify);
	}
	m_LockFlushNotify.Unlock();
#else
	READ_WRITE_BARRIER
	while (*(volatile int*)&m_nFlush)
	{
	#if CRY_PLATFORM_WINDOWS
		const HWND hWnd = GetRenderWindowHandle();
		if (hWnd)
		{
			gEnv->pSystem->PumpWindowMessage(true, hWnd);
		}
		CrySleep(0);
	#endif
		READ_WRITE_BARRIER
	}
#endif

	END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.waitForRender);
}

// Must be executed from render thread
void SRenderThread::WaitFlushCond()
{
	CRY_PROFILE_FUNCTION(PROFILE_RENDERER);
	START_PROFILE_RT_SCOPE();

#ifdef USE_LOCKS_FOR_FLUSH_SYNC
	m_LockFlushNotify.Lock();
	while (!*(volatile int*)&m_nFlush)
	{
		m_FlushCondition.Wait(m_LockFlushNotify);
	}
	m_LockFlushNotify.Unlock();
#else
	READ_WRITE_BARRIER
	while (!*(volatile int*)&m_nFlush)
	{
		if (m_bQuit)
			break;
		CrySleep(0);
		READ_WRITE_BARRIER
	}
#endif

	END_PROFILE_PLUS_RT(SRenderStatistics::Write().m_Summary.waitForMain);
}

#undef m_nCurThreadFill
#undef m_nCurThreadProcess
