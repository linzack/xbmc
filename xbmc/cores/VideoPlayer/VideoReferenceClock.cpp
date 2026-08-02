/*
 *  Copyright (C) 2005-2018 Team Kodi
 *  This file is part of Kodi - https://kodi.tv
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSES/README.md for more information.
 */
#include "VideoReferenceClock.h"

#include "ServiceBroker.h"
#include "settings/Settings.h"
#include "settings/SettingsComponent.h"
#include "utils/MathUtils.h"
#include "utils/TimeUtils.h"
#include "utils/log.h"
#include "windowing/GraphicContext.h"
#include "windowing/VideoSync.h"
#include "windowing/WinSystem.h"

#include <mutex>

CVideoReferenceClock::CVideoReferenceClock() : CThread("RefClock")
{
  m_SystemFrequency = CurrentHostFrequency();
  m_ClockSpeed = 1.0;
  m_TotalMissedVblanks = 0;
  m_UseVblank = false;

  m_CurrTime = 0;
  m_LastIntTime = 0;
  m_CurrTimeFract = 0.0;
  m_RefreshRate = 0.0;
  m_MissedVblanks = 0;
  m_VblankTime = 0;
  m_vsyncStopEvent.Reset();

  Start();
}

CVideoReferenceClock::~CVideoReferenceClock()
{
  m_bStop = true;
  m_vsyncStopEvent.Set();
  StopThread();
}

void CVideoReferenceClock::Start()
{
  if(CServiceBroker::GetSettingsComponent()->GetSettings()->GetBool(CSettings::SETTING_VIDEOPLAYER_USEDISPLAYASCLOCK) && !IsRunning())
    Create();
}

void CVideoReferenceClock::UpdateClock(int NrVBlanks, uint64_t time)
{
  std::unique_lock lock(m_CritSection);

  m_VblankTime = time;
  UpdateClockInternal(NrVBlanks, true);
}

void CVideoReferenceClock::Process()
{
  bool SetupSuccess = false;
  int64_t Now;

  while(!m_bStop)
  {
    m_pVideoSync = CServiceBroker::GetWinSystem()->GetVideoSync(this);

    if (m_pVideoSync)
    {
      SetupSuccess = m_pVideoSync->Setup();
      UpdateRefreshrate();
    }

    std::unique_lock SingleLock(m_CritSection);
    Now = CurrentHostCounter();
    m_CurrTime = Now;
    m_LastIntTime = m_CurrTime;
    m_CurrTimeFract = 0.0;
    m_ClockSpeed = 1.0;
    m_TotalMissedVblanks = 0;
    m_MissedVblanks = 0;

    if (SetupSuccess)
    {
      m_UseVblank = true;          //tell other threads we're using vblank as clock
      m_VblankTime = Now;          //initialize the timestamp of the last vblank
      SingleLock.unlock();

      // we might got signalled while we did not wait
      if (!m_vsyncStopEvent.Signaled())
      {
        //run the clock
        m_pVideoSync->Run(m_vsyncStopEvent);
        m_vsyncStopEvent.Reset();
      }
    }
    else
    {
      SingleLock.unlock();
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Setup failed, falling back to CurrentHostCounter()");
    }

    SingleLock.lock();
    m_UseVblank = false;                       //we're back to using the systemclock
    SingleLock.unlock();

    //clean up the vblank clock
    if (m_pVideoSync)
    {
      m_pVideoSync->Cleanup();
      m_pVideoSync.reset();
    }

    if (!SetupSuccess)
      break;
  }
}

//this is called from the vblank run function and from CVideoReferenceClock::Wait in case of a late update
void CVideoReferenceClock::UpdateClockInternal(int NrVBlanks, bool CheckMissed)
{
  if (CheckMissed) //set to true from the vblank run function, set to false from Wait and GetTime
  {
    if (NrVBlanks < m_MissedVblanks) //if this is true the vblank detection in the run function is wrong
      CLog::Log(
          LOGDEBUG,
          "CVideoReferenceClock: detected {} vblanks, missed {}, refreshrate might have changed",
          NrVBlanks, m_MissedVblanks);

    NrVBlanks -= m_MissedVblanks; //subtract the vblanks we missed
    m_MissedVblanks = 0;
  }
  else
  {
    m_MissedVblanks += NrVBlanks;      //tell the vblank clock how many vblanks it missed
    m_TotalMissedVblanks += NrVBlanks; //for the codec information screen
    m_VblankTime += m_SystemFrequency * static_cast<int64_t>(NrVBlanks) / MathUtils::round_int(m_RefreshRate); //set the vblank time forward
  }

  if (NrVBlanks > 0) //update the clock with the adjusted frequency if we have any vblanks
  {
    double increment = UpdateInterval() * NrVBlanks;
    double integer   = floor(increment);
    m_CurrTime      += static_cast<int64_t>(integer + 0.5); //make sure it gets correctly converted to int

    //accumulate what we lost due to rounding in m_CurrTimeFract, then add the integer part of that to m_CurrTime
    m_CurrTimeFract += increment - integer;
    integer          = floor(m_CurrTimeFract);
    m_CurrTime      += static_cast<int64_t>(integer + 0.5);
    m_CurrTimeFract -= integer;
  }
}

double CVideoReferenceClock::UpdateInterval() const
{
  return m_ClockSpeed / m_RefreshRate * static_cast<double>(m_SystemFrequency);
}

//called from dvdclock to get the time
int64_t CVideoReferenceClock::GetTime(bool interpolated /* = true*/)
{
  std::unique_lock SingleLock(m_CritSection);

  //when using vblank, get the time from that, otherwise use the systemclock
  if (m_UseVblank)
  {
    int64_t  NextVblank;
    int64_t  Now;

    Now = CurrentHostCounter();        //get current system time
    NextVblank = TimeOfNextVblank();   //get time when the next vblank should happen

    // [EVAL_SHADOW][19621a9 & ce12636] Insertion-only first-deadline jitter window evaluation (retains baseline 13LL macro)
    // Note: NextVblank in the while loop below uses the compiled MAXVBLANKDELAY (13LL on pre-ce12636 baseline),
    // while nextVblank20 is a locally computed diagnostic comparison point.
    int64_t vblankPeriod = m_SystemFrequency / MathUtils::round_int(m_RefreshRate);
    int64_t nextVblank13 = m_VblankTime + (vblankPeriod * 13LL / 10LL);
    int64_t nextVblank20 = m_VblankTime + (vblankPeriod * 20LL / 10LL);

    if (Now >= nextVblank13 && Now < nextVblank20)
    {
      static std::atomic<int64_t> lastJitterLogTime{0};
      auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      int64_t prevLogTime = lastJitterLogTime.load();
      if (nowMs - prevLogTime >= 1000 && lastJitterLogTime.compare_exchange_strong(prevLogTime, nowMs))
      {
        CLog::Log(LOGDEBUG, "[EVAL_SHADOW][VBLANK_JITTER_WINDOW] Delta: {}ns | Baseline 13LL Speculate: YES, Fixed 20LL Speculate: NO. "
                  "(20LL tolerance absorbs hardware jitter).", (int64_t)(Now - nextVblank13));
      }
    }

    if (Now >= NextVblank && m_ClockSpeed <= 0.01)
    {
      static std::atomic<int64_t> lastPauseLogTime{0};
      auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      int64_t prevLogTime = lastPauseLogTime.load();
      if (nowMs - prevLogTime >= 1000 && lastPauseLogTime.compare_exchange_strong(prevLogTime, nowMs))
      {
        int64_t specDelta = static_cast<int64_t>(Now - NextVblank);
        CLog::Log(LOGWARNING, "[EVAL_SHADOW][PAUSE_DRIFT] Speculative jump on PAUSE (speed: {:.2f}, delta: {} ticks). Baseline advances clock during pause.",
                  m_ClockSpeed, specDelta);
      }
    }

    double jumpMs = static_cast<double>(Now - NextVblank) * 1000.0 / m_SystemFrequency;
    if (jumpMs > 1000.0)
    {
      static std::atomic<int64_t> lastStallLogTime{0};
      auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      int64_t prevLogTime = lastStallLogTime.load();
      if (nowMs - prevLogTime >= 1000 && lastStallLogTime.compare_exchange_strong(prevLogTime, nowMs))
      {
        CLog::Log(LOGWARNING, "[EVAL_SHADOW][MASSIVE_STALL] Speculative jump of {:.2f}ms predicted. Hardware sync loss likely.", jumpMs);
      }
    }

    while(Now >= NextVblank)  //keep looping until the next vblank is in the future
    {
      UpdateClockInternal(1, false); //update clock when next vblank should have happened already
      NextVblank = TimeOfNextVblank(); //get time when the next vblank should happen
    }

    if (interpolated)
    {
      //interpolate from the last time the clock was updated
      double elapsed = static_cast<double>(Now - m_VblankTime) * m_ClockSpeed;
      //don't interpolate more than 2 vblank periods
      elapsed = std::min(elapsed, UpdateInterval() * 2.0);

      //make sure the clock doesn't go backwards
      int64_t intTime = m_CurrTime + static_cast<int64_t>(elapsed);
      if (intTime > m_LastIntTime)
        m_LastIntTime = intTime;

      return m_LastIntTime;
    }
    else
    {
      return m_CurrTime;
    }
  }
  else
  {
    return CurrentHostCounter();
  }
}

void CVideoReferenceClock::SetSpeed(double Speed)
{
  std::unique_lock SingleLock(m_CritSection);
  //VideoPlayer can change the speed to fit the rereshrate
  if (m_UseVblank)
  {
    if (Speed != m_ClockSpeed)
    {
      m_ClockSpeed = Speed;
      CLog::Log(LOGDEBUG, "CVideoReferenceClock: Clock speed {:0.2f} %", m_ClockSpeed * 100.0);
    }
  }
  else if (Speed != m_ClockSpeed)
  {
    CLog::Log(LOGDEBUG, "[EVAL_SHADOW][SETSPEED_IGNORED] SetSpeed {:.2f} ignored because m_UseVblank is false! Baseline drops speed update.", Speed);
  }
}

double CVideoReferenceClock::GetSpeed()
{
  std::unique_lock SingleLock(m_CritSection);

  // [EVAL_SHADOW][19621a9] Log when GetSpeed falls back to 1.0 due to m_UseVblank == false (omits equal-value m_ClockSpeed == 1.0 case)
  if (!m_UseVblank && m_ClockSpeed != 1.0)
  {
    CLog::Log(LOGDEBUG, "[EVAL_SHADOW][GETSPEED_FALLBACK] GetSpeed returning 1.0 fallback instead of actual m_ClockSpeed {:.2f}", m_ClockSpeed);
  }
  if (m_UseVblank)
    return m_ClockSpeed;
  else
    return 1.0;
}

void CVideoReferenceClock::UpdateRefreshrate()
{
  std::unique_lock SingleLock(m_CritSection);
  m_RefreshRate = static_cast<double>(m_pVideoSync->GetFps());
  m_ClockSpeed = 1.0;

  CLog::Log(LOGDEBUG, "CVideoReferenceClock: Detected refreshrate: {:.3f} hertz", m_RefreshRate);
}

//VideoPlayer needs to know the refreshrate for matching the fps of the video playing to it
double CVideoReferenceClock::GetRefreshRate(double* interval /*= NULL*/)
{
  std::unique_lock SingleLock(m_CritSection);

  if (m_UseVblank)
  {
    if (interval)
      *interval = m_ClockSpeed / m_RefreshRate;

    return m_RefreshRate;
  }
  else
    return -1;
}

#define MAXVBLANKDELAY 13LL
//guess when the next vblank should happen,
//based on the refreshrate and when the previous one happened
//increase that by 30% to allow for errors
int64_t CVideoReferenceClock::TimeOfNextVblank() const
{
  return m_VblankTime + (m_SystemFrequency / MathUtils::round_int(m_RefreshRate) * MAXVBLANKDELAY / 10LL);
}

//for the codec information screen
bool CVideoReferenceClock::GetClockInfo(int& MissedVblanks, double& ClockSpeed, double& RefreshRate) const
{
  std::unique_lock SingleLock(m_CritSection);

  if (m_UseVblank)
  {
    MissedVblanks = m_TotalMissedVblanks;
    ClockSpeed = m_ClockSpeed;
    RefreshRate = m_RefreshRate;
    return true;
  }
  return false;
}
