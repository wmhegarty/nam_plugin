#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

#include "Colors.h"

// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralAmpModeler.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on
#include "architecture.hpp"
#include <NAM/get_dsp.h>
#include "NeuralAmpModelerControls.h"

using namespace iplug;
using namespace igraphics;

const double kDCBlockerFrequency = 5.0;

// Styles
const IVColorSpec colorSpec{
  DEFAULT_BGCOLOR, // Background
  PluginColors::NAM_THEMECOLOR, // Foreground
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.3f), // Pressed
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.4f), // Frame
  PluginColors::MOUSEOVER, // Highlight
  DEFAULT_SHCOLOR, // Shadow
  PluginColors::NAM_THEMECOLOR, // Extra 1
  COLOR_RED, // Extra 2 --> color for clipping in meters
  PluginColors::NAM_THEMECOLOR.WithContrast(0.1f), // Extra 3
};

const IVStyle style =
  IVStyle{true, // Show label
          true, // Show value
          colorSpec,
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text5
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
          DEFAULT_HIDE_CURSOR,
          DEFAULT_DRAW_FRAME,
          false,
          DEFAULT_EMBOSS,
          0.2f,
          2.f,
          DEFAULT_SHADOW_OFFSET,
          DEFAULT_WIDGET_FRAC,
          DEFAULT_WIDGET_ANGLE};

const IVStyle titleStyle =
  DEFAULT_STYLE.WithValueText(IText(30, COLOR_WHITE, "Michroma-Regular")).WithDrawFrame(false).WithShadowOffset(2.f);

EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                              EMsgBoxType type)
{
#ifdef OS_MAC
  // macOS is backwards?
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}


NeuralAmpModeler::NeuralAmpModeler(const InstanceInfo& info)
: iplug::Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  // Generate unique instance ID from memory address
  std::stringstream ss;
  ss << std::hex << reinterpret_cast<uintptr_t>(this);
  mInstanceId = ss.str();

  _InitToneStack();
  _LoadPreferences();
  //nam::activations::Activation::enable_fast_tanh();
  GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
  GetParam(kToneBass)->InitDouble("Bass", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneMid)->InitDouble("Middle", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneTreble)->InitDouble("Treble", 5.0, 0.0, 10.0, 0.1);
  GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
  GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
  GetParam(kEQActive)->InitBool("ToneStack", true);
  GetParam(kOutNorm)->InitBool("OutNorm", true);
  GetParam(kIRToggle)->InitBool("IRToggle", true);
  // NAM and IR navigation parameters for host automation (trigger when value goes to 1)
  GetParam(kNAMPrev)->InitBool("NAM Prev", false);
  GetParam(kNAMNext)->InitBool("NAM Next", false);
  GetParam(kIRPrev)->InitBool("IR Prev", false);
  GetParam(kIRNext)->InitBool("IR Next", false);

  // Read-only filename display parameters (for host querying like GigPerformer)
  // These use a dummy double 0-1 range but display the filename via DisplayFunc
  GetParam(kNAMName)->InitDouble("NAM Name", 0.0, 0.0, 1.0, 0.01);
  GetParam(kNAMName)->SetDisplayFunc([this](double, WDL_String& str) {
    if (mCurrentNAMName.GetLength() > 0)
      str.Set(mCurrentNAMName.Get());
    else
      str.Set("(no model)");
    std::cerr << "NAM DisplayFunc called, returning: " << str.Get() << std::endl;
  });
  GetParam(kIRName)->InitDouble("IR Name", 0.0, 0.0, 1.0, 0.01);
  GetParam(kIRName)->SetDisplayFunc([this](double, WDL_String& str) {
    if (mCurrentIRName.GetLength() > 0)
      str.Set(mCurrentIRName.Get());
    else
      str.Set("(no IR)");
    std::cerr << "IR DisplayFunc called, returning: " << str.Get() << std::endl;
  });

  // Rackspace and slot parameters - read these in GP Script to match with state file
  // Note: Hosts normalize to 0.0-1.0, so GP Script must denormalize: value = 1 + normalizedValue * 15
  GetParam(kRackspace)->InitInt("Rackspace", 1, 1, 16);
  GetParam(kSlot)->InitInt("Slot", 1, 1, 16);

  mNoiseGateTrigger.AddListener(&mNoiseGateGain);

  mMakeGraphicsFunc = [&]() {

#ifdef OS_IOS
    auto scaleFactor = GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT) * 0.85f;
#else
    auto scaleFactor = 1.0f;
#endif

    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, scaleFactor);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);
    pGraphics->EnableMultiTouch(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto gearSVG = pGraphics->LoadSVG(GEAR_FN);
    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);
    const auto modelIconSVG = pGraphics->LoadSVG(MODEL_ICON_FN);
    const auto irIconOnSVG = pGraphics->LoadSVG(IR_ICON_ON_FN);
    const auto irIconOffSVG = pGraphics->LoadSVG(IR_ICON_OFF_FN);

    const auto backgroundBitmap = pGraphics->LoadBitmap(BACKGROUND_FN);
    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto linesBitmap = pGraphics->LoadBitmap(LINES_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadBitmap(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadBitmap(SLIDESWITCHHANDLE_FN);
    const auto meterBackgroundBitmap = pGraphics->LoadBitmap(METERBACKGROUND_FN);

    const auto b = pGraphics->GetBounds();
    const auto mainArea = b.GetPadded(-20);
    const auto contentArea = mainArea.GetPadded(-10);
    const auto titleHeight = 50.0f;
    const auto titleArea = contentArea.GetFromTop(titleHeight);

    // Areas for knobs
    const auto knobsPad = 20.0f;
    const auto knobsExtraSpaceBelowTitle = 25.0f;
    const auto knobHeight = 120.f;
    const auto singleKnobPad = -2.0f;
    const auto knobsArea = contentArea.GetFromTop(knobHeight)
                             .GetReducedFromLeft(knobsPad)
                             .GetReducedFromRight(knobsPad)
                             .GetVShifted(titleHeight + knobsExtraSpaceBelowTitle);
    const auto inputKnobArea = knobsArea.GetGridCell(0, kInputLevel, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto noiseGateArea = knobsArea.GetGridCell(0, kNoiseGateThreshold, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto bassKnobArea = knobsArea.GetGridCell(0, kToneBass, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto midKnobArea = knobsArea.GetGridCell(0, kToneMid, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto trebleKnobArea = knobsArea.GetGridCell(0, kToneTreble, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto outputKnobArea = knobsArea.GetGridCell(0, kOutputLevel, 1, numKnobs).GetPadded(-singleKnobPad);

    const auto ngToggleArea =
      noiseGateArea.GetVShifted(noiseGateArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto eqToggleArea = midKnobArea.GetVShifted(midKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto outNormToggleArea =
      outputKnobArea.GetVShifted(midKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);

    // Areas for model and IR
    const auto fileWidth = 200.0f;
    const auto fileHeight = 30.0f;
    const auto irYOffset = 38.0f;
    const auto modelArea =
      contentArea.GetFromBottom((2.0f * fileHeight)).GetFromTop(fileHeight).GetMidHPadded(fileWidth).GetVShifted(-1);
    const auto modelIconArea = modelArea.GetFromLeft(30).GetTranslated(-40, 10);
    const auto irArea = modelArea.GetVShifted(irYOffset);
    const auto irSwitchArea = irArea.GetFromLeft(30.0f).GetHShifted(-40.0f).GetScaledAboutCentre(0.6f);

    // Areas for meters
    const auto inputMeterArea = contentArea.GetFromLeft(30).GetHShifted(-20).GetMidVPadded(100).GetVShifted(-25);
    const auto outputMeterArea = contentArea.GetFromRight(30).GetHShifted(20).GetMidVPadded(100).GetVShifted(-25);

    // Misc Areas
    const auto settingsButtonArea = mainArea.GetFromTRHC(50, 50).GetCentredInside(20, 20);
    // Slot number control - small number box to the left of settings button
    const auto slotArea = settingsButtonArea.GetHShifted(-35).GetScaledAboutCentre(1.5f);

    // Model loader button
    auto loadModelCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        // Sets mNAMPath and mStagedNAM
        const std::string msg = _StageModel(fileName);
        // TODO error messages like the IR loader.
        if (msg.size())
        {
          std::stringstream ss;
          ss << "Failed to load NAM model. Message:\n\n" << msg;
          _ShowMessageBox(GetUI(), ss.str().c_str(), "Failed to load model!", kMB_OK);
        }
        std::cout << "Loaded: " << fileName.Get() << std::endl;
      }
    };

    // IR loader button
    auto loadIRCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        mIRPath = fileName;
        const dsp::wav::LoadReturnCode retCode = _StageIR(fileName);
        if (retCode != dsp::wav::LoadReturnCode::SUCCESS)
        {
          std::stringstream message;
          message << "Failed to load IR file " << fileName.Get() << ":\n";
          message << dsp::wav::GetMsgForLoadReturnCode(retCode);

          _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
        }
      }
    };

    pGraphics->AttachBackground(BACKGROUND_FN);
    pGraphics->AttachControl(new IBitmapControl(b, linesBitmap));
    pGraphics->AttachControl(new IVLabelControl(titleArea, "NEURAL AMP MODELER", titleStyle));
    pGraphics->AttachControl(new ISVGControl(modelIconArea, modelIconSVG));

#ifdef NAM_PICK_DIRECTORY
    const std::string defaultNamFileString = "Select model directory...";
    const std::string defaultIRString = "Select IR directory...";
#else
    const std::string defaultNamFileString = "Select model...";
    const std::string defaultIRString = "Select IR...";
#endif
    pGraphics->AttachControl(new NAMFileBrowserControl(modelArea, kMsgTagClearModel, defaultNamFileString.c_str(),
                                                       "nam", loadModelCompletionHandler, style, fileSVG, crossSVG,
                                                       leftArrowSVG, rightArrowSVG, fileBackgroundBitmap,
                                                       mLastNAMDirectory.Get()),
                             kCtrlTagModelFileBrowser);
    pGraphics->AttachControl(new ISVGSwitchControl(irSwitchArea, {irIconOffSVG, irIconOnSVG}, kIRToggle));
    pGraphics->AttachControl(
      new NAMFileBrowserControl(irArea, kMsgTagClearIR, defaultIRString.c_str(), "wav", loadIRCompletionHandler, style,
                                fileSVG, crossSVG, leftArrowSVG, rightArrowSVG, fileBackgroundBitmap,
                                mLastIRDirectory.Get()),
      kCtrlTagIRFileBrowser);
    pGraphics->AttachControl(
      new NAMSwitchControl(ngToggleArea, kNoiseGateActive, "Noise Gate", style, switchHandleBitmap));
    pGraphics->AttachControl(new NAMSwitchControl(eqToggleArea, kEQActive, "EQ", style, switchHandleBitmap));
    pGraphics->AttachControl(
      new NAMSwitchControl(outNormToggleArea, kOutNorm, "Normalize", style, switchHandleBitmap), kCtrlTagOutNorm);

    // The knobs
    pGraphics->AttachControl(new NAMKnobControl(inputKnobArea, kInputLevel, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(noiseGateArea, kNoiseGateThreshold, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(
      new NAMKnobControl(bassKnobArea, kToneBass, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(midKnobArea, kToneMid, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(trebleKnobArea, kToneTreble, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(new NAMKnobControl(outputKnobArea, kOutputLevel, "", style, knobBackgroundBitmap));

    // The meters
    pGraphics->AttachControl(new NAMMeterControl(inputMeterArea, meterBackgroundBitmap, style), kCtrlTagInputMeter);
    pGraphics->AttachControl(new NAMMeterControl(outputMeterArea, meterBackgroundBitmap, style), kCtrlTagOutputMeter);

    // CPU usage monitor - centered between title and knobs
    // Title is 50px, knobs start at 75px (50 + 25 gap), so place monitor in that 25px gap
    const auto monitorY = contentArea.T + titleHeight + 2.5f;  // Start 2.5px below title
    const auto usageMonitorArea = IRECT(contentArea.MW() - 57.5f, monitorY, contentArea.MW() + 57.5f, monitorY + 20.0f);
    pGraphics->AttachControl(new NAMUsageMonitorControl(usageMonitorArea), kCtrlTagUsageMonitor);

    // Settings/help/about box
    pGraphics->AttachControl(new NAMCircleButtonControl(
      settingsButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagSettingsBox)->As<NAMAboutBoxControl>()->HideAnimated(false);
      },
      gearSVG));

    pGraphics->AttachControl(new NAMAboutBoxControl(b, backgroundBitmap, style), kCtrlTagSettingsBox)->Hide(true);

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(true);
      pControl->SetMouseOverWhenDisabled(true);
    });

    pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetMouseEventsWhenDisabled(false);
  };
}

NeuralAmpModeler::~NeuralAmpModeler()
{
  _DeleteStateFile();
  _DeallocateIOPointers();
}

void NeuralAmpModeler::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  const size_t numChannelsExternalIn = (size_t)NInChansConnected();
  const size_t numChannelsExternalOut = (size_t)NOutChansConnected();
  const size_t numChannelsInternal = kNumChannelsInternal;
  const size_t numFrames = (size_t)nFrames;
  const double sampleRate = GetSampleRate();

  // Disable floating point denormals
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  _PrepareBuffers(numChannelsInternal, numFrames);
  // Input is collapsed to mono in preparation for the NAM.
  _ProcessInput(inputs, numFrames, numChannelsExternalIn, numChannelsInternal);
  _ApplyDSPStaging();
  const bool noiseGateActive = GetParam(kNoiseGateActive)->Value();
  const bool toneStackActive = GetParam(kEQActive)->Value();

  // Noise gate trigger
  sample** triggerOutput = mInputPointers;
  if (noiseGateActive)
  {
    const double time = 0.01;
    const double threshold = GetParam(kNoiseGateThreshold)->Value(); // GetParam...
    const double ratio = 0.1; // Quadratic...
    const double openTime = 0.005;
    const double holdTime = 0.01;
    const double closeTime = 0.05;
    const dsp::noise_gate::TriggerParams triggerParams(time, threshold, ratio, openTime, holdTime, closeTime);
    mNoiseGateTrigger.SetParams(triggerParams);
    mNoiseGateTrigger.SetSampleRate(sampleRate);
    triggerOutput = mNoiseGateTrigger.Process(mInputPointers, numChannelsInternal, numFrames);
  }

  if (mModel != nullptr)
  {
    if (mIntermediateIn.size() < nFrames) {
      mIntermediateIn.resize(nFrames, 0);
      mIntermediateOut.resize(nFrames, 0);
    }

    for (int s = 0; s < nFrames; ++s)
      mIntermediateIn[s] = triggerOutput[0][s];

    mModel->process(mIntermediateIn.data(), mIntermediateOut.data(), nFrames);

    for (int s = 0; s < nFrames; ++s)
      mOutputPointers[0][s] = mIntermediateOut[s];
    // Normalize loudness
    if (GetParam(kOutNorm)->Value())
    {
      _NormalizeModelOutput(mOutputPointers, numChannelsInternal, numFrames);
    }
  }
  else
  {
    _FallbackDSP(triggerOutput, mOutputPointers, numChannelsInternal, numFrames);
  }
  // Apply the noise gate
  sample** gateGainOutput =
    noiseGateActive ? mNoiseGateGain.Process(mOutputPointers, numChannelsInternal, numFrames) : mOutputPointers;

  sample** toneStackOutPointers = (toneStackActive && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutput, numChannelsInternal, numFrames)
                                    : gateGainOutput;

  sample** irPointers = toneStackOutPointers;
  if (mIR != nullptr && GetParam(kIRToggle)->Value())
    irPointers = mIR->Process(toneStackOutPointers, numChannelsInternal, numFrames);

  // And the HPF for DC offset (Issue 271)
  const double highPassCutoffFreq = kDCBlockerFrequency;
  // const double lowPassCutoffFreq = 20000.0;
  const recursive_linear_filter::HighPassParams highPassParams(sampleRate, highPassCutoffFreq);
  // const recursive_linear_filter::LowPassParams lowPassParams(sampleRate, lowPassCutoffFreq);
  mHighPass.SetParams(highPassParams);
  // mLowPass.SetParams(lowPassParams);
  sample** hpfPointers = mHighPass.Process(irPointers, numChannelsInternal, numFrames);
  // sample** lpfPointers = mLowPass.Process(hpfPointers, numChannelsInternal, numFrames);

  // restore previous floating point state
  std::feupdateenv(&fe_state);

  // Let's get outta here
  // This is where we exit mono for whatever the output requires.
  _ProcessOutput(hpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // _ProcessOutput(lpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // * Output of input leveling (inputs -> mInputPointers),
  // * Output of output leveling (mOutputPointers -> outputs)
  _UpdateMeters(mInputPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
}

void NeuralAmpModeler::OnReset()
{
  const auto sampleRate = GetSampleRate();
  const int maxBlockSize = GetBlockSize();

  // Tail is because the HPF DC blocker has a decay.
  // 10 cycles should be enough to pass the VST3 tests checking tail behavior.
  // I'm ignoring the model & IR, but it's not the end of the world.
  const int tailCycles = 10;
  SetTailSize(tailCycles * (int)(sampleRate / kDCBlockerFrequency));
  mInputSender.Reset(sampleRate);
  mOutputSender.Reset(sampleRate);
  // If there is a model or IR loaded, they need to be checked for resampling.
  _ResetModelAndIR(sampleRate, GetBlockSize());
  mToneStack->Reset(sampleRate, maxBlockSize);
  _UpdateLatency();

  // Register this instance and update the instance number parameter
  _WriteStateFile();
}

void NeuralAmpModeler::OnIdle()
{
  mInputSender.TransmitData(*this);
  mOutputSender.TransmitData(*this);

  if (mNewModelLoadedInDSP)
  {
    if (auto* pGraphics = GetUI())
      pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetDisabled(!mModel->HasLoudness());

    mNewModelLoadedInDSP = false;
  }

  // Update CPU usage monitor (once per second)
  mCpuIdleCounter++;
  if (mCpuIdleCounter >= 30)
  {
    mCpuIdleCounter = 0;
    mCpuLoad = _GetProcessCpuUsage();
    mCpuLoadSmoothed = mCpuLoadSmoothed + kSmoothingFactor * (mCpuLoad - mCpuLoadSmoothed);

    if (auto* pGraphics = GetUI())
    {
      if (auto* pMonitor = pGraphics->GetControlWithTag(kCtrlTagUsageMonitor))
      {
        auto* monitor = pMonitor->As<NAMUsageMonitorControl>();
        monitor->SetCpuLoad(static_cast<float>(mCpuLoadSmoothed));
        monitor->SetGPUActive(mModel != nullptr && mModel->IsGPU());
      }

      // Check if window scale has changed and save it
      float currentScale = pGraphics->GetDrawScale();
      if (std::abs(currentScale - mLastAppliedScale) > 0.01f)
      {
        mLastAppliedScale = currentScale;
        mSavedWindowScale = currentScale;
        _SavePreferences();
      }
    }
  }
}

bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  // If this isn't here when unserializing, then we know we're dealing with something before v0.8.0.
  WDL_String header("###NeuralAmpModeler###"); // Don't change this!
  chunk.PutStr(header.Get());
  // Plugin version, so we can load legacy serialized states in the future!
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  // Model directory (don't serialize the model itself; we'll just load it again
  // when we unserialize)
  chunk.PutStr(mNAMPath.Get());
  chunk.PutStr(mIRPath.Get());
  return SerializeParams(chunk);
}

int NeuralAmpModeler::UnserializeState(const IByteChunk& chunk, int startPos)
{
  WDL_String header;
  int pos = startPos;
  pos = chunk.GetStr(header, pos);
  // Unseralization:
  {
    // Handle legacy plugin serialized states:
    // In v0.7.9, this was the NAM filepath. So, if we dont' get the expected header, then we can attempt to unserialize
    // as v0.7.9:
    const char* kExpectedHeader = "###NeuralAmpModeler###";
    if (strcmp(header.Get(), kExpectedHeader) == 0)
    {
      pos = _UnserializeStateCurrent(chunk, pos);
    }
    else
    {
      pos = _UnserializeStateLegacy_0_7_9(chunk, startPos);
    }
  }
  if (mNAMPath.GetLength())
    _StageModel(mNAMPath);
  if (mIRPath.GetLength())
    _StageIR(mIRPath);
  return pos;
}

void NeuralAmpModeler::OnUIOpen()
{
  Plugin::OnUIOpen();

  // Apply saved window scale if different from default
  if (auto* pGraphics = GetUI())
  {
    if (mSavedWindowScale != 1.0f)
    {
      pGraphics->Resize(PLUG_WIDTH, PLUG_HEIGHT, mSavedWindowScale);
      mLastAppliedScale = mSavedWindowScale;
    }
    else
    {
      mLastAppliedScale = pGraphics->GetDrawScale();
    }
  }

  if (mNAMPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
    // If it's not loaded yet, then mark as failed.
    // If it's yet to be loaded, then the completion handler will set us straight once it runs.
    if (mModel == nullptr && mStagedModel == nullptr)
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);
  }

  if (mIRPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
    if (mIR == nullptr && mStagedIR == nullptr)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  if (mModel != nullptr)
    GetUI()->GetControlWithTag(kCtrlTagOutNorm)->SetDisabled(!mModel->HasLoudness());
}

void NeuralAmpModeler::OnParamChange(int paramIdx)
{
  switch (paramIdx)
  {
    case kToneBass: mToneStack->SetParam("bass", GetParam(paramIdx)->Value()); break;
    case kToneMid: mToneStack->SetParam("middle", GetParam(paramIdx)->Value()); break;
    case kToneTreble: mToneStack->SetParam("treble", GetParam(paramIdx)->Value()); break;
    case kNAMPrev:
      if (GetParam(kNAMPrev)->Bool())
      {
        _NavigateNAM(-1);
        // Reset the parameter back to false
        GetParam(kNAMPrev)->Set(0.0);
      }
      break;
    case kNAMNext:
      if (GetParam(kNAMNext)->Bool())
      {
        _NavigateNAM(+1);
        GetParam(kNAMNext)->Set(0.0);
      }
      break;
    case kIRPrev:
      if (GetParam(kIRPrev)->Bool())
      {
        _NavigateIR(-1);
        GetParam(kIRPrev)->Set(0.0);
      }
      break;
    case kIRNext:
      if (GetParam(kIRNext)->Bool())
      {
        _NavigateIR(+1);
        GetParam(kIRNext)->Set(0.0);
      }
      break;
    default: break;
  }
}

void NeuralAmpModeler::OnParamChangeUI(int paramIdx, EParamSource source)
{
  if (auto pGraphics = GetUI())
  {
    bool active = GetParam(paramIdx)->Bool();

    switch (paramIdx)
    {
      case kNoiseGateActive: pGraphics->GetControlWithParamIdx(kNoiseGateThreshold)->SetDisabled(!active); break;
      case kEQActive:
        pGraphics->ForControlInGroup("EQ_KNOBS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kIRToggle: pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser)->SetDisabled(!active);
      default: break;
    }
  }
}

bool NeuralAmpModeler::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModel: mShouldRemoveModel = true; return true;
    case kMsgTagClearIR: mShouldRemoveIR = true; return true;
    case kMsgTagHighlightColor:
    {
      mHighLightColor.Set((const char*)pData);

      if (GetUI())
      {
        GetUI()->ForStandardControlsFunc([&](IControl* pControl) {
          if (auto* pVectorBase = pControl->As<IVectorBase>())
          {
            IColor color = IColor::FromColorCodeStr(mHighLightColor.Get());

            pVectorBase->SetColor(kX1, color);
            pVectorBase->SetColor(kPR, color.WithOpacity(0.3f));
            pVectorBase->SetColor(kFR, color.WithOpacity(0.4f));
            pVectorBase->SetColor(kX3, color.WithContrast(0.1f));
          }
          pControl->GetUI()->SetAllControlsDirty();
        });
      }

      return true;
    }
    default: return false;
  }
}

// Private methods ============================================================

void NeuralAmpModeler::_AllocateIOPointers(const size_t nChans)
{
  if (mInputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mInputPointers without freeing");
  mInputPointers = new sample*[nChans];
  if (mInputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mOutputPointers without freeing");
  mOutputPointers = new sample*[nChans];
  if (mOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_ApplyDSPStaging()
{
  // Remove marked modules
  if (mShouldRemoveModel)
  {
    mModel = nullptr;
    mNAMPath.Set("");
    mShouldRemoveModel = false;
    _UpdateLatency();
  }
  if (mShouldRemoveIR)
  {
    mIR = nullptr;
    mIRPath.Set("");
    mShouldRemoveIR = false;
  }
  // Move things from staged to live
  if (mStagedModel != nullptr)
  {
    // Move from staged to active DSP
    mModel = std::move(mStagedModel);
    mStagedModel = nullptr;
    mNewModelLoadedInDSP = true;
    _UpdateLatency();
  }
  if (mStagedIR != nullptr)
  {
    mIR = std::move(mStagedIR);
    mStagedIR = nullptr;
  }
}

void NeuralAmpModeler::_DeallocateIOPointers()
{
  if (mInputPointers != nullptr)
  {
    delete[] mInputPointers;
    mInputPointers = nullptr;
  }
  if (mInputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
  {
    delete[] mOutputPointers;
    mOutputPointers = nullptr;
  }
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels,
                                    const size_t numFrames)
{
  for (auto c = 0; c < numChannels; c++)
    for (auto s = 0; s < numFrames; s++)
      mOutputArray[c][s] = mInputArray[c][s];
}

void NeuralAmpModeler::_NormalizeModelOutput(iplug::sample** buffer, const size_t numChannels, const size_t numFrames)
{
  if (!mModel)
    return;
  if (!mModel->HasLoudness())
    return;
  const double loudness = mModel->GetLoudness();
  const double targetLoudness = -18.0;
  const double gain = pow(10.0, (targetLoudness - loudness) / 20.0);
  for (size_t c = 0; c < numChannels; c++)
  {
    for (size_t f = 0; f < numFrames; f++)
    {
      buffer[c][f] *= gain;
    }
  }
}

void NeuralAmpModeler::_ResetModelAndIR(const double sampleRate, const int maxBlockSize)
{
  // Model
  if (mStagedModel != nullptr)
  {
    mStagedModel->Reset(sampleRate, maxBlockSize);
  }
  else if (mModel != nullptr)
  {
    mModel->Reset(sampleRate, maxBlockSize);
  }

  // IR
  if (mStagedIR != nullptr)
  {
    const double irSampleRate = mStagedIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mStagedIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
  else if (mIR != nullptr)
  {
    const double irSampleRate = mIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
}

std::string NeuralAmpModeler::_StageModel(const WDL_String& modelPath)
{
  WDL_String previousNAMPath = mNAMPath;
  try
  {
    auto dspPath = std::filesystem::u8path(modelPath.Get());
    std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath, 256, 256);
    std::unique_ptr<ResamplingNAM> temp = std::make_unique<ResamplingNAM>(std::move(model), GetSampleRate());
    temp->Reset(GetSampleRate(), GetBlockSize());
    mStagedModel = std::move(temp);
    mNAMPath = modelPath;
    // Update the filename parameter for host querying
    mCurrentNAMName.Set(_GetFilenameWithoutExtension(modelPath.Get()).c_str());
    _WriteStateFile();
    // Save the directory for next time
    mLastNAMDirectory = modelPath;
    mLastNAMDirectory.remove_filepart();
    _SavePreferences();
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
  }
  catch (std::runtime_error& e)
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);

    if (mStagedModel != nullptr)
    {
      mStagedModel = nullptr;
    }
    mNAMPath = previousNAMPath;
    std::cerr << "Failed to read DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
    return e.what();
  }
  return "";
}

dsp::wav::LoadReturnCode NeuralAmpModeler::_StageIR(const WDL_String& irPath)
{
  // FIXME it'd be better for the path to be "staged" as well. Just in case the
  // path and the model got caught on opposite sides of the fence...
  WDL_String previousIRPath = mIRPath;
  const double sampleRate = GetSampleRate();
  dsp::wav::LoadReturnCode wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
  try
  {
    auto irPathU8 = std::filesystem::u8path(irPath.Get());
    mStagedIR = std::make_unique<dsp::ImpulseResponse>(irPathU8.string().c_str(), sampleRate);
    wavState = mStagedIR->GetWavState();
  }
  catch (std::runtime_error& e)
  {
    wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
    std::cerr << "Caught unhandled exception while attempting to load IR:" << std::endl;
    std::cerr << e.what() << std::endl;
  }

  if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
  {
    mIRPath = irPath;
    // Update the filename parameter for host querying
    mCurrentIRName.Set(_GetFilenameWithoutExtension(irPath.Get()).c_str());
    _WriteStateFile();
    // Save the directory for next time
    mLastIRDirectory = irPath;
    mLastIRDirectory.remove_filepart();
    _SavePreferences();
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
  }
  else
  {
    if (mStagedIR != nullptr)
    {
      mStagedIR = nullptr;
    }
    mIRPath = previousIRPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  return wavState;
}

size_t NeuralAmpModeler::_GetBufferNumChannels() const
{
  // Assumes input=output (no mono->stereo effects)
  return mInputArray.size();
}

size_t NeuralAmpModeler::_GetBufferNumFrames() const
{
  if (_GetBufferNumChannels() == 0)
    return 0;
  return mInputArray[0].size();
}

void NeuralAmpModeler::_InitToneStack()
{
  // If you want to customize the tone stack, then put it here!
  mToneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();
}
void NeuralAmpModeler::_PrepareBuffers(const size_t numChannels, const size_t numFrames)
{
  const bool updateChannels = numChannels != _GetBufferNumChannels();
  const bool updateFrames = updateChannels || (_GetBufferNumFrames() != numFrames);
  //  if (!updateChannels && !updateFrames)  // Could we do this?
  //    return;

  if (updateChannels)
  {
    _PrepareIOPointers(numChannels);
    mInputArray.resize(numChannels);
    mOutputArray.resize(numChannels);
  }
  if (updateFrames)
  {
    for (auto c = 0; c < mInputArray.size(); c++)
    {
      mInputArray[c].resize(numFrames);
      std::fill(mInputArray[c].begin(), mInputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mOutputArray.size(); c++)
    {
      mOutputArray[c].resize(numFrames);
      std::fill(mOutputArray[c].begin(), mOutputArray[c].end(), 0.0);
    }
  }
  // Would these ever get changed by something?
  for (auto c = 0; c < mInputArray.size(); c++)
    mInputPointers[c] = mInputArray[c].data();
  for (auto c = 0; c < mOutputArray.size(); c++)
    mOutputPointers[c] = mOutputArray[c].data();
}

void NeuralAmpModeler::_PrepareIOPointers(const size_t numChannels)
{
  _DeallocateIOPointers();
  _AllocateIOPointers(numChannels);
}

void NeuralAmpModeler::_ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn,
                                     const size_t nChansOut)
{
  // We'll assume that the main processing is mono for now. We'll handle dual amps later.
  if (nChansOut != 1)
  {
    std::stringstream ss;
    ss << "Expected mono output, but " << nChansOut << " output channels are requested!";
    throw std::runtime_error(ss.str());
  }

  // On the standalone, we can probably assume that the user has plugged into only one input and they expect it to be
  // carried straight through. Don't apply any division over nCahnsIn because we're just "catching anything out there."
  // However, in a DAW, it's probably something providing stereo, and we want to take the average in order to avoid
  // doubling the loudness.
#ifdef APP_API
  const double gain = pow(10.0, GetParam(kInputLevel)->Value() / 20.0);
#else
  const double gain = pow(10.0, GetParam(kInputLevel)->Value() / 20.0) / (float)nChansIn;
#endif
  // Assume _PrepareBuffers() was already called
  for (size_t c = 0; c < nChansIn; c++)
    for (size_t s = 0; s < nFrames; s++)
      if (c == 0)
        mInputArray[0][s] = gain * inputs[c][s];
      else
        mInputArray[0][s] += gain * inputs[c][s];
}

void NeuralAmpModeler::_ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames,
                                      const size_t nChansIn, const size_t nChansOut)
{
  const double gain = pow(10.0, GetParam(kOutputLevel)->Value() / 20.0);
  // Assume _PrepareBuffers() was already called
  if (nChansIn != 1)
    throw std::runtime_error("Plugin is supposed to process in mono.");
  // Broadcast the internal mono stream to all output channels.
  const size_t cin = 0;
  for (auto cout = 0; cout < nChansOut; cout++)
    for (auto s = 0; s < nFrames; s++)
#ifdef APP_API // Ensure valid output to interface
      outputs[cout][s] = std::clamp(gain * inputs[cin][s], -1.0, 1.0);
#else // In a DAW, other things may come next and should be able to handle large
      // values.
      outputs[cout][s] = gain * inputs[cin][s];
#endif
}

int NeuralAmpModeler::_UnserializeStateCurrent(const IByteChunk& chunk, int pos)
{
  WDL_String version;
  pos = chunk.GetStr(version, pos);
  // Post-v0.7.9 legacy loading here once needed:
  // ...

  // Current version loading:
  pos = chunk.GetStr(mNAMPath, pos);
  pos = chunk.GetStr(mIRPath, pos);
  pos = UnserializeParams(chunk, pos);
  return pos;
}

int NeuralAmpModeler::_UnserializeStateLegacy_0_7_9(const IByteChunk& chunk, int startPos)
{
  WDL_String dir;
  int pos = startPos;
  pos = chunk.GetStr(mNAMPath, pos);
  pos = chunk.GetStr(mIRPath, pos);
  auto unserialize = [&](const IByteChunk& chunk, int startPos) {
    // cf IPluginBase::UnserializeParams(const IByteChunk& chunk, int startPos)

    // These are the parameter names, in the order that they were serialized in v0.7.9.
    std::vector<std::string> oldParamNames{
      "Input", "Gate", "Bass", "Middle", "Treble", "Output", "NoiseGateActive", "ToneStack", "OutNorm", "IRToggle"};
    // These are their current names.
    // IF YOU CHANGE THE NAMES OF THE PARAMETERS, THEN YOU NEED TO UPDATE THIS!
    std::unordered_map<std::string, std::string> newNames{{"Gate", "Threshold"}};
    auto getParamByOldName = [&, newNames](std::string& oldName) {
      std::string name = newNames.find(oldName) != newNames.end() ? newNames.at(oldName) : oldName;
      // Could use a map but eh
      for (int i = 0; i < kNumParams; i++)
      {
        IParam* param = GetParam(i);
        if (strcmp(param->GetName(), name.c_str()) == 0)
        {
          return param;
        }
      }
      return (IParam*)nullptr;
    };
    TRACE
    int pos = startPos;
    ENTER_PARAMS_MUTEX
    int i = 0;
    for (auto it = oldParamNames.begin(); it != oldParamNames.end(); ++it, i++)
    {
      // Here's the change: instead of assuming that we can iterate through the parameters, we look for the one that now
      // holds this info.
      // IParam* pParam = mParams.Get(i);
      IParam* pParam = getParamByOldName(*it);

      double v = 0.0;
      pos = chunk.Get(&v, pos);
      // It's possible that future versions will not have all of the params of previous versions. If that's the case,
      // then this is a null ptr and we skip it.
      if (pParam)
      {
        pParam->Set(v);
        Trace(TRACELOC, "%d %s %f", i, pParam->GetName(), pParam->Value());
      }
      else
      {
        Trace(TRACELOC, "%d NOT-FOUND", i);
      }
    }
    OnParamReset(kPresetRecall);
    LEAVE_PARAMS_MUTEX
    return pos;
  };
  pos = unserialize(chunk, pos);
  return pos;
}

void NeuralAmpModeler::_UpdateLatency()
{
  int latency = 0;
  if (mModel)
  {
    latency += mModel->GetLatency();

    // Next power of 2 for the current block size
    unsigned int v = GetBlockSize();
    v--;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v++;

    latency += ::std::min(4096u, v);
  }
  // Other things that add latency here...

  // Feels weird to have to do this.
  if (GetLatency() != latency)
  {
    SetLatency(latency);
  }
}

void NeuralAmpModeler::_UpdateMeters(sample** inputPointer, sample** outputPointer, const size_t nFrames,
                                     const size_t nChansIn, const size_t nChansOut)
{
  // Right now, we didn't specify MAXNC when we initialized these, so it's 1.
  const int nChansHack = 1;
  mInputSender.ProcessBlock(inputPointer, (int)nFrames, kCtrlTagInputMeter, nChansHack);
  mOutputSender.ProcessBlock(outputPointer, (int)nFrames, kCtrlTagOutputMeter, nChansHack);
}

WDL_String NeuralAmpModeler::_GetPreferencesPath() const
{
  WDL_String path;
  DesktopPath(path);
  // Go up from Desktop to user home, then to Application Support
  path.remove_filepart();
  path.Append("/Library/Application Support/NeuralAmpModeler/");
  return path;
}

void NeuralAmpModeler::_SavePreferences()
{
  // Get the user-assigned rackspace and slot numbers for preferences
  int rackspace = static_cast<int>(GetParam(kRackspace)->Value());
  if (rackspace < 1 || rackspace > 16)
    rackspace = 1;
  int slot = static_cast<int>(GetParam(kSlot)->Value());
  if (slot < 1 || slot > 16)
    slot = 1;

  WDL_String prefsPath = _GetPreferencesPath();

  // Create directory if it doesn't exist
  std::filesystem::create_directories(prefsPath.Get());

  // Use rackspace/slot-specific preferences file
  WDL_String prefsFile = prefsPath;
  prefsFile.AppendFormatted(64, "prefs_rackspace_%d_slot_%d.txt", rackspace, slot);

  std::ofstream file(prefsFile.Get());
  if (file.is_open())
  {
    file << "NAMDir=" << mLastNAMDirectory.Get() << std::endl;
    file << "IRDir=" << mLastIRDirectory.Get() << std::endl;
    file << "NAMFile=" << mNAMPath.Get() << std::endl;
    file << "IRFile=" << mIRPath.Get() << std::endl;
    file << "WindowScale=" << mSavedWindowScale << std::endl;
    file.close();
  }
}

void NeuralAmpModeler::_LoadPreferences()
{
  // Get the user-assigned rackspace and slot numbers for preferences
  int rackspace = static_cast<int>(GetParam(kRackspace)->Value());
  if (rackspace < 1 || rackspace > 16)
    rackspace = 1;
  int slot = static_cast<int>(GetParam(kSlot)->Value());
  if (slot < 1 || slot > 16)
    slot = 1;

  WDL_String prefsPath = _GetPreferencesPath();

  // Use rackspace/slot-specific preferences file
  WDL_String prefsFile = prefsPath;
  prefsFile.AppendFormatted(64, "prefs_rackspace_%d_slot_%d.txt", rackspace, slot);

  WDL_String savedNAMFile, savedIRFile;

  std::ifstream file(prefsFile.Get());
  if (file.is_open())
  {
    std::string line;
    while (std::getline(file, line))
    {
      if (line.rfind("NAMDir=", 0) == 0)
      {
        mLastNAMDirectory.Set(line.substr(7).c_str());
      }
      else if (line.rfind("IRDir=", 0) == 0)
      {
        mLastIRDirectory.Set(line.substr(6).c_str());
      }
      else if (line.rfind("NAMFile=", 0) == 0)
      {
        savedNAMFile.Set(line.substr(8).c_str());
      }
      else if (line.rfind("IRFile=", 0) == 0)
      {
        savedIRFile.Set(line.substr(7).c_str());
      }
      else if (line.rfind("WindowScale=", 0) == 0)
      {
        mSavedWindowScale = static_cast<float>(std::stod(line.substr(12)));
        // Clamp to reasonable range
        if (mSavedWindowScale < 0.5f) mSavedWindowScale = 0.5f;
        if (mSavedWindowScale > 4.0f) mSavedWindowScale = 4.0f;
      }
    }
    file.close();
  }

  // Load the saved model and IR if they exist
  if (savedNAMFile.GetLength() > 0 && std::filesystem::exists(savedNAMFile.Get()))
  {
    _StageModel(savedNAMFile);
  }
  if (savedIRFile.GetLength() > 0 && std::filesystem::exists(savedIRFile.Get()))
  {
    _StageIR(savedIRFile);
  }
}

double NeuralAmpModeler::_GetProcessCpuUsage()
{
  // Get this process's CPU time
  mach_port_t task = mach_task_self();
  task_thread_times_info_data_t threadTimesInfo;
  mach_msg_type_number_t count = TASK_THREAD_TIMES_INFO_COUNT;

  if (task_info(task, TASK_THREAD_TIMES_INFO, (task_info_t)&threadTimesInfo, &count) != KERN_SUCCESS)
  {
    return 0.0;
  }

  // Convert to microseconds
  uint64_t userTime = threadTimesInfo.user_time.seconds * 1000000ULL + threadTimesInfo.user_time.microseconds;
  uint64_t systemTime = threadTimesInfo.system_time.seconds * 1000000ULL + threadTimesInfo.system_time.microseconds;
  uint64_t totalCpuTime = userTime + systemTime;

  auto now = std::chrono::steady_clock::now();

  if (!mCpuCheckInitialized)
  {
    mPrevUserTime = userTime;
    mPrevSystemTime = systemTime;
    mPrevCpuCheckTime = now;
    mCpuCheckInitialized = true;
    return 0.0;
  }

  // Calculate elapsed wall time in microseconds
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - mPrevCpuCheckTime).count();
  if (elapsed == 0)
  {
    return 0.0;
  }

  // Calculate CPU time used since last check
  uint64_t cpuTimeDiff = (userTime - mPrevUserTime) + (systemTime - mPrevSystemTime);

  mPrevUserTime = userTime;
  mPrevSystemTime = systemTime;
  mPrevCpuCheckTime = now;

  // CPU usage = CPU time used / wall time elapsed
  // This gives usage as a fraction of one core; clamp to 1.0 for display
  double usage = static_cast<double>(cpuTimeDiff) / static_cast<double>(elapsed);
  return std::min(1.0, usage);
}

void NeuralAmpModeler::ProcessMidiMsg(const IMidiMsg& msg)
{
  // Handle MIDI CC messages
  if (msg.StatusMsg() == IMidiMsg::kControlChange)
  {
    int cc = msg.mData1;
    int value = msg.mData2;
    double normalized = value / 127.0;  // 0.0 to 1.0

    // Continuous controls (knobs)
    switch (cc)
    {
      case kMidiCCInput:
      {
        // Input: -20 to +20 dB
        double dbValue = -20.0 + normalized * 40.0;
        GetParam(kInputLevel)->Set(dbValue);
        SendParameterValueFromAPI(kInputLevel, dbValue, true);
        return;
      }
      case kMidiCCNoiseGate:
      {
        // Noise Gate Threshold: -100 to 0 dB
        double dbValue = -100.0 + normalized * 100.0;
        GetParam(kNoiseGateThreshold)->Set(dbValue);
        SendParameterValueFromAPI(kNoiseGateThreshold, dbValue, true);
        return;
      }
      case kMidiCCBass:
      {
        // Bass: 0 to 10
        double eqValue = normalized * 10.0;
        GetParam(kToneBass)->Set(eqValue);
        SendParameterValueFromAPI(kToneBass, eqValue, true);
        return;
      }
      case kMidiCCMid:
      {
        // Mid: 0 to 10
        double eqValue = normalized * 10.0;
        GetParam(kToneMid)->Set(eqValue);
        SendParameterValueFromAPI(kToneMid, eqValue, true);
        return;
      }
      case kMidiCCTreble:
      {
        // Treble: 0 to 10
        double eqValue = normalized * 10.0;
        GetParam(kToneTreble)->Set(eqValue);
        SendParameterValueFromAPI(kToneTreble, eqValue, true);
        return;
      }
      case kMidiCCOutput:
      {
        // Output: -40 to +40 dB
        double dbValue = -40.0 + normalized * 80.0;
        GetParam(kOutputLevel)->Set(dbValue);
        SendParameterValueFromAPI(kOutputLevel, dbValue, true);
        return;
      }
    }

    // Trigger controls (buttons) - only trigger on value > 63
    if (value > 63)
    {
      switch (cc)
      {
        case kMidiCCNAMPrev:
          _NavigateNAM(-1);
          return;
        case kMidiCCNAMNext:
          _NavigateNAM(+1);
          return;
        case kMidiCCIRPrev:
          _NavigateIR(-1);
          return;
        case kMidiCCIRNext:
          _NavigateIR(+1);
          return;
        case kMidiCCNoiseGateToggle:
        {
          bool current = GetParam(kNoiseGateActive)->Bool();
          GetParam(kNoiseGateActive)->Set(!current);
          SendParameterValueFromAPI(kNoiseGateActive, !current ? 1.0 : 0.0, true);
          return;
        }
        case kMidiCCEQToggle:
        {
          bool current = GetParam(kEQActive)->Bool();
          GetParam(kEQActive)->Set(!current);
          SendParameterValueFromAPI(kEQActive, !current ? 1.0 : 0.0, true);
          return;
        }
        case kMidiCCIRToggle:
        {
          bool current = GetParam(kIRToggle)->Bool();
          GetParam(kIRToggle)->Set(!current);
          SendParameterValueFromAPI(kIRToggle, !current ? 1.0 : 0.0, true);
          return;
        }
        case kMidiCCNormalize:
        {
          bool current = GetParam(kOutNorm)->Bool();
          GetParam(kOutNorm)->Set(!current);
          SendParameterValueFromAPI(kOutNorm, !current ? 1.0 : 0.0, true);
          return;
        }
      }
    }
  }
}

void NeuralAmpModeler::_ScanNAMDirectory()
{
  mNAMFiles.clear();
  mCurrentNAMIndex = -1;

  if (mLastNAMDirectory.GetLength() == 0)
    return;

  std::filesystem::path dirPath(mLastNAMDirectory.Get());
  if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath))
    return;

  // Scan for .nam files
  for (const auto& entry : std::filesystem::directory_iterator(dirPath))
  {
    if (entry.is_regular_file())
    {
      auto ext = entry.path().extension().string();
      // Convert extension to lowercase for comparison
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".nam")
      {
        mNAMFiles.push_back(entry.path().string());
      }
    }
  }

  // Sort alphabetically
  std::sort(mNAMFiles.begin(), mNAMFiles.end());

  // Find current file index if a model is loaded
  if (mNAMPath.GetLength() > 0)
  {
    std::string currentPath = mNAMPath.Get();
    for (size_t i = 0; i < mNAMFiles.size(); i++)
    {
      if (mNAMFiles[i] == currentPath)
      {
        mCurrentNAMIndex = static_cast<int>(i);
        break;
      }
    }
  }

  mNAMDirectoryScanned = true;
}

void NeuralAmpModeler::_ScanIRDirectory()
{
  mIRFiles.clear();
  mCurrentIRIndex = -1;

  if (mLastIRDirectory.GetLength() == 0)
    return;

  std::filesystem::path dirPath(mLastIRDirectory.Get());
  if (!std::filesystem::exists(dirPath) || !std::filesystem::is_directory(dirPath))
    return;

  // Scan for .wav files
  for (const auto& entry : std::filesystem::directory_iterator(dirPath))
  {
    if (entry.is_regular_file())
    {
      auto ext = entry.path().extension().string();
      // Convert extension to lowercase for comparison
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      if (ext == ".wav")
      {
        mIRFiles.push_back(entry.path().string());
      }
    }
  }

  // Sort alphabetically
  std::sort(mIRFiles.begin(), mIRFiles.end());

  // Find current file index if an IR is loaded
  if (mIRPath.GetLength() > 0)
  {
    std::string currentPath = mIRPath.Get();
    for (size_t i = 0; i < mIRFiles.size(); i++)
    {
      if (mIRFiles[i] == currentPath)
      {
        mCurrentIRIndex = static_cast<int>(i);
        break;
      }
    }
  }

  mIRDirectoryScanned = true;
}

void NeuralAmpModeler::_NavigateNAM(int direction)
{
  // Rescan if needed
  if (!mNAMDirectoryScanned || mNAMFiles.empty())
  {
    _ScanNAMDirectory();
  }

  if (mNAMFiles.empty())
    return;

  // Calculate new index with wrapping
  if (mCurrentNAMIndex < 0)
  {
    mCurrentNAMIndex = (direction > 0) ? 0 : static_cast<int>(mNAMFiles.size()) - 1;
  }
  else
  {
    mCurrentNAMIndex += direction;
    if (mCurrentNAMIndex < 0)
      mCurrentNAMIndex = static_cast<int>(mNAMFiles.size()) - 1;
    else if (mCurrentNAMIndex >= static_cast<int>(mNAMFiles.size()))
      mCurrentNAMIndex = 0;
  }

  // Load the model
  WDL_String path(mNAMFiles[mCurrentNAMIndex].c_str());
  _StageModel(path);
}

void NeuralAmpModeler::_NavigateIR(int direction)
{
  // Rescan if needed
  if (!mIRDirectoryScanned || mIRFiles.empty())
  {
    _ScanIRDirectory();
  }

  if (mIRFiles.empty())
    return;

  // Calculate new index with wrapping
  if (mCurrentIRIndex < 0)
  {
    mCurrentIRIndex = (direction > 0) ? 0 : static_cast<int>(mIRFiles.size()) - 1;
  }
  else
  {
    mCurrentIRIndex += direction;
    if (mCurrentIRIndex < 0)
      mCurrentIRIndex = static_cast<int>(mIRFiles.size()) - 1;
    else if (mCurrentIRIndex >= static_cast<int>(mIRFiles.size()))
      mCurrentIRIndex = 0;
  }

  // Load the IR
  WDL_String path(mIRFiles[mCurrentIRIndex].c_str());
  _StageIR(path);
}

std::string NeuralAmpModeler::_GetFilenameWithoutExtension(const char* path)
{
  if (!path || !*path)
    return "";

  std::filesystem::path p(path);
  return p.stem().string();
}

void NeuralAmpModeler::_WriteStateFile()
{
  // Only write if we have at least one name set (avoid overwriting with empty values on startup)
  if (mCurrentNAMName.GetLength() == 0 && mCurrentIRName.GetLength() == 0)
    return;

  // Get the user-assigned rackspace and slot numbers
  int rackspace = static_cast<int>(GetParam(kRackspace)->Value());
  if (rackspace < 1 || rackspace > 16)
    rackspace = 1;
  int slot = static_cast<int>(GetParam(kSlot)->Value());
  if (slot < 1 || slot > 16)
    slot = 1;

  // Write to rackspace/slot-specific file: rackspace_1_slot_1.txt, etc.
  WDL_String statePath = _GetPreferencesPath();
  std::filesystem::create_directories(statePath.Get());

  WDL_String slotFile = statePath;
  slotFile.AppendFormatted(64, "rackspace_%d_slot_%d.txt", rackspace, slot);

  std::ofstream stateFile(slotFile.Get());
  if (stateFile.is_open())
  {
    stateFile << "NAM=" << mCurrentNAMName.Get() << std::endl;
    stateFile << "IR=" << mCurrentIRName.Get() << std::endl;
    stateFile.close();
  }
}

void NeuralAmpModeler::_DeleteStateFile()
{
  // Delete the rackspace/slot file for this instance
  int rackspace = static_cast<int>(GetParam(kRackspace)->Value());
  if (rackspace < 1 || rackspace > 16)
    return;
  int slot = static_cast<int>(GetParam(kSlot)->Value());
  if (slot < 1 || slot > 16)
    return;

  WDL_String statePath = _GetPreferencesPath();
  WDL_String slotFile = statePath;
  slotFile.AppendFormatted(64, "rackspace_%d_slot_%d.txt", rackspace, slot);
  std::filesystem::remove(slotFile.Get());
}
