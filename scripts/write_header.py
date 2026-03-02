#!/usr/bin/env python3
"""Write the new slim F1LampUsermod.h that uses the C network layer."""
import pathlib, textwrap

content = textwrap.dedent('''\
    #pragma once
    /*
     * F1LampUsermod.h  -  WLED usermod for F1 live-timing reaction lamp.
     *
     * Includes ONLY wled.h + F1NetWork.h (pure-C interface).
     * All ESP-IDF HTTP/WS code lives in F1NetWork.cpp (never includes wled.h).
     *
     * Preset ID defaults (from the user real WLED export):
     *   IDLE=12  SESSION_START=20  GREEN=15  YELLOW=16
     *   SC=13    VSC=22            RED=14    CHEQUERED=23
     */

    #include "wled.h"
    #include "F1NetWork.h"

    class F1LampUsermod : public Usermod {
    public:
      static const char _name[];
      static const char _enabled[];

    private:
      /* -------------------------------------------------------
         Configurable (persisted in WLED flash / JSON config)
         ------------------------------------------------------- */
      bool    enabled            = true;
      /* forceState 0=live, 1-8 maps to F1NetState IDLE..CHEQUERED */
      uint8_t forceState         = 0;
      uint8_t presetIdle         = 12;
      uint8_t presetSessionStart = 20;
      uint8_t presetGreen        = 15;
      uint8_t presetYellow       = 16;
      uint8_t presetSafetyCar    = 13;
      uint8_t presetVirtualSC    = 22;
      uint8_t presetRedFlag      = 14;
      uint8_t presetChequered    = 23;

      /* -------------------------------------------------------
         Runtime
         ------------------------------------------------------- */
      bool       _initDone    = false;
      F1NetState _lastApplied = F1ST_UNKNOWN;
      uint8_t    _lastForce   = 0;

      /* Pending state from the IDF WS task, applied in loop() */
      static volatile F1NetState s_pending;
      static volatile bool       s_pendingValid;

      /* -------------------------------------------------------
         Helpers
         ------------------------------------------------------- */
      uint8_t presetForState(F1NetState s) const {
        switch (s) {
          case F1ST_IDLE:          return presetIdle;
          case F1ST_SESSION_START: return presetSessionStart;
          case F1ST_GREEN:         return presetGreen;
          case F1ST_YELLOW:        return presetYellow;
          case F1ST_SAFETY_CAR:    return presetSafetyCar;
          case F1ST_VIRTUAL_SC:    return presetVirtualSC;
          case F1ST_RED_FLAG:      return presetRedFlag;
          case F1ST_CHEQUERED:     return presetChequered;
          default: return 0;
        }
      }

      static const char* stateLabel(F1NetState s) {
        switch (s) {
          case F1ST_IDLE:          return "No session";
          case F1ST_SESSION_START: return "Session starting";
          case F1ST_GREEN:         return "Green flag";
          case F1ST_YELLOW:        return "Yellow flag";
          case F1ST_SAFETY_CAR:    return "Safety Car";
          case F1ST_VIRTUAL_SC:    return "Virtual SC";
          case F1ST_RED_FLAG:      return "Red flag";
          case F1ST_CHEQUERED:     return "Chequered flag";
          default:                 return "---";
        }
      }

      void applyStatePreset(F1NetState state) {
        if (state == _lastApplied) return;
        _lastApplied = state;
        uint8_t pid  = presetForState(state);
        DEBUG_PRINTF("[F1Lamp] -> %s  preset=%d\\n", stateLabel(state), pid);
        if (pid > 0) applyPreset(pid);
      }

      /* Callback from IDF WS task – keep it minimal */
      static void onStateChange(F1NetState state) {
        s_pending      = state;
        s_pendingValid = true;
      }

    /* ================================================
       Usermod interface
       ================================================ */
    public:
      void setup() override {
        f1net_setCallback(F1LampUsermod::onStateChange);
        f1net_setup();
        _initDone = true;
        DEBUG_PRINTLN("[F1Lamp] Ready");
      }

      void loop() override {
        if (!enabled || !_initDone) return;

        /* Force / test mode */
        if (forceState > 0) {
          F1NetState forced = (F1NetState)(forceState - 1);
          if (forced != _lastApplied || forceState != _lastForce) {
            _lastForce = forceState;
            applyStatePreset(forced);
          }
          return;
        }
        if (_lastForce != 0) {
          _lastForce   = 0;
          _lastApplied = F1ST_UNKNOWN;
        }

        /* Apply state change from IDF WS task */
        if (s_pendingValid) {
          s_pendingValid = false;
          applyStatePreset((F1NetState)s_pending);
        }

        /* Drive network reconnect / keepalive */
        f1net_loop();
      }

      void addToJsonInfo(JsonObject& root) override {
        JsonObject user = root["u"];
        if (user.isNull()) user = root.createNestedObject("u");
        user.createNestedArray(F("F1 Status")).add(stateLabel(_lastApplied));
        JsonArray r2 = user.createNestedArray(F("F1 Stream"));
        if      (forceState > 0)       r2.add(F("TEST MODE"));
        else if (f1net_isConnected())  r2.add(F("live"));
        else                           r2.add(F("offline"));
      }

      void appendConfigData() override {
        oappend(SET_F(
          "addInfo(\'F1Lamp:forceState\',1,\'0=live 1=idle 2=start 3=green 4=yellow 5=SC 6=VSC 7=red 8=chq\');"
          "addInfo(\'F1Lamp:presetIdle\',1,\'WLED preset/playlist ID (0=skip)\');"
          "addInfo(\'F1Lamp:presetSessionStart\',1,\'WLED preset/playlist ID\');"
          "addInfo(\'F1Lamp:presetGreen\',1,\'WLED preset/playlist ID\');"
          "addInfo(\'F1Lamp:presetYellow\',1,\'WLED preset/playlist ID\');"
          "addInfo(\'F1Lamp:presetSafetyCar\',1,\'WLED preset/playlist ID\');"
          "addInfo(\'F1Lamp:presetVirtualSC\',1,\'WLED preset/playlist ID\');"
          "addInfo(\'F1Lamp:presetRedFlag\',1,\'WLED preset/playlist ID\');"
          "addInfo(\'F1Lamp:presetChequered\',1,\'WLED preset/playlist ID\');"
        ));
      }

      void addToConfig(JsonObject& root) override {
        JsonObject top = root.createNestedObject(FPSTR(_name));
        top[FPSTR(_enabled)]      = enabled;
        top["forceState"]         = forceState;
        top["presetIdle"]         = presetIdle;
        top["presetSessionStart"] = presetSessionStart;
        top["presetGreen"]        = presetGreen;
        top["presetYellow"]       = presetYellow;
        top["presetSafetyCar"]    = presetSafetyCar;
        top["presetVirtualSC"]    = presetVirtualSC;
        top["presetRedFlag"]      = presetRedFlag;
        top["presetChequered"]    = presetChequered;
      }

      bool readFromConfig(JsonObject& root) override {
        JsonObject top = root[FPSTR(_name)];
        if (top.isNull()) return false;
        bool prev = enabled;
        getJsonValue(top[FPSTR(_enabled)],      enabled);
        getJsonValue(top["forceState"],          forceState);
        getJsonValue(top["presetIdle"],          presetIdle);
        getJsonValue(top["presetSessionStart"],  presetSessionStart);
        getJsonValue(top["presetGreen"],         presetGreen);
        getJsonValue(top["presetYellow"],        presetYellow);
        getJsonValue(top["presetSafetyCar"],     presetSafetyCar);
        getJsonValue(top["presetVirtualSC"],     presetVirtualSC);
        getJsonValue(top["presetRedFlag"],       presetRedFlag);
        getJsonValue(top["presetChequered"],     presetChequered);
        if (prev && !enabled) f1net_disconnect();
        return true;
      }

      uint16_t getId() override { return USERMOD_ID_UNSPECIFIED; }
    };

    const char F1LampUsermod::_name[]    PROGMEM = "F1Lamp";
    const char F1LampUsermod::_enabled[] PROGMEM = "enabled";

    volatile F1NetState F1LampUsermod::s_pending      = F1ST_IDLE;
    volatile bool       F1LampUsermod::s_pendingValid  = false;
''')

# Fix escaped single-quotes back to actual single-quotes in the C string literals
content = content.replace("\\'", "'")

dst = pathlib.Path(r'C:\Users\Victor\source\repos\f1-lamp\usermod\F1LampUsermod.h')
dst.write_text(content, encoding='utf-8', newline='\n')
print(f'Written {len(content.splitlines())} lines to {dst}')
