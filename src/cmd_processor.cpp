#include "cmd_processor.h"

#define MAX_HISTORY 10

CMDS::CMDS(String cmd, void (*callback)(String &cmd)) : _callback(callback) {
  _cmd = cmd;
  _args = "*s";
  _callback2 = nullptr;
}

CMDS::CMDS(String cmd, String args, void (*callback)(String &cmd, String args, void* values)) : _callback2(callback) {
  _cmd = String(cmd);
  _args = String(args);
  _callback = nullptr;
}

String CMDS::getCmd() {
  return _cmd;
}
String CMDS::getArgs() {
  return _args;
}

bool CMDS::handle(String cmd, String args) {
  if(cmd.startsWith(_cmd)) {
    if(_callback) {
      if(args.isEmpty()) {
        _callback(cmd);
      } else {
        _callback(cmd + " " + args);
      }
    } else if(_callback2) {
      _callback2(cmd, _args, nullptr);
    }
    return true;
  }
  return false;
}

///////////////////////////////////////////////////////

CMD_PROCESSOR::CMD_PROCESSOR(Stream* cmd_stream) : _cmd_stream(cmd_stream) {
  _history_pos = -1;
}

bool CMD_PROCESSOR::registerCmd(String cmd, void (*callback)(String &cmd)) {
  CMDS* p_cmd = new CMDS(cmd, callback);
  _cmds.push_back(p_cmd);
  std::sort(_cmds.begin(), _cmds.end());
  return false;
}

bool CMD_PROCESSOR::registerKey(char key, void (*callback)(char key)) {
  return false;
}

void CMD_PROCESSOR::splitCmdline() {
  String helper(_current_input);
  helper.trim();
  int idx = helper.indexOf(" ");
  if(idx > 0) {
    _cmd = helper.substring(0, idx);
    _args = helper.substring(idx + 1);
  } else {
    _cmd = helper;
    _args = "";
  }
}

void CMD_PROCESSOR::process() {
  if(!_cmd_stream->available()) {
    return;
  }
  char c = _cmd_stream->read();
  if(handleKey(c)) {
    return;
  }
  if((c >= 0x20) && (c <= 0x7E)) {
    if(_current_pos == _current_input.length()) {
      _current_input += c;
      _cmd_stream->print(c);
    } else {
      _current_input = _current_input.substring(0, _current_pos) + c + _current_input.substring(_current_pos);
      _cmd_stream->printf("\r%s \b\33[%dD", _current_input.c_str(), _current_input.length() - _current_pos - 1);
    }
    _current_pos++;
  } else if((c == '\r') || (c == '\n')) {
    if(_current_pos != _current_input.length()) {
      _cmd_stream->printf("\33[%dC", _current_input.length() - _current_pos);
    }
    handleCrLf();
    _current_pos = 0;
  } else if((c == 8) || (c == 0x7f)) { // backspace
    if(!_current_input.isEmpty()) {
      _current_input.remove(_current_pos - 1, 1);
      if(_current_pos == _current_input.length() + 1) {
        _cmd_stream->print("\b \b");
      } else {
        _cmd_stream->printf("\r%s \b\33[%dD", _current_input.c_str(), _current_input.length() - _current_pos + 1);
      }
      _current_pos--;
    }
  }
  splitCmdline();
  _last_char = c;
}

bool CMD_PROCESSOR::handleKey(char c) {
  static bool esc = false;
  static bool sci = false;
  static bool csi = false;
  static String csi_str("");
  static char special_char = 0;
  if(esc) {
    if(c == 'Z') {
      sci = true;
    } else if(c == 0x5B) {
      csi = true;
    } else if(c == 0x4F) { // F1-F4
      special_char = c;
    } else if((c >= 0x40) && (c < 0x7E)) {
      // TODO handle single esc
      //_cmd_stream->printf("esc %02X\r\n", c);
      _cmd_stream->printf("\a");
    }
    esc = false;
    return true;
  } else if(sci) {
    // TODO handle cursors etc
    //_cmd_stream->printf("sci %02X\r\n", c);
    _cmd_stream->printf("\a");
    sci = false;
    return true;
  } else if(special_char) {
      // TODO handle F1-4, umlauts etc
    //_cmd_stream->printf("special %02x %02X\r\n", special_char, c);
    _cmd_stream->printf("\a");
    special_char = 0;
    return true;
  } else if(csi) {
    csi_str += c;
    if((c >=0x40) && (c <= 0x7E)) {
      // TODO handle cursors etc
      if(csi_str == "A") { // cursor up
        //_cmd_stream->printf("curup %d %d %d\r\n", _history_pos, _history.size());
        if(_history_pos < ((int8_t)_history.size() - 1)) {
          if(_history_pos == -1) {
            _input_cache = _current_input;
          }
          _history_pos++;
          _current_input = _history.at(_history_pos);
          _cmd_stream->printf("\33[2K\r%s", _current_input.c_str());
          _current_pos = _current_input.length();
          splitCmdline();
        } else {
          _cmd_stream->printf("\a");
        }
      } else if(csi_str == "B") { // cursor down
        //_cmd_stream->printf("curdown %d\r\n", _history_pos);
        if(_history_pos > -1) {
          _history_pos--;
          if(_history_pos == -1) {
            _current_input = _input_cache;
          } else {
            _current_input = _history.at(_history_pos);
          }
          _cmd_stream->printf("\33[2K\r%s", _current_input.c_str());
          _current_pos = _current_input.length();
          splitCmdline();
        } else {
          _cmd_stream->printf("\a");
        }
      } else if(csi_str == "C") { // cursor right
        if(_current_pos < _current_input.length()) {
          _cmd_stream->printf("\33[C");
          _current_pos++;
        }
      } else if(csi_str == "D") { // cursor left
        if(_current_pos) {
          _cmd_stream->printf("\33[D");
          _current_pos--;
        }
      } else {
        _cmd_stream->printf("\a");
        //_cmd_stream->printf("csi %s\r\n", csi_str.c_str());
      }
      csi_str = "";
      csi = false;
    }
    return true;
  }
  if(c == 0x1B) {
    esc = true;
    return true;
  } else if(c == 0xC3) {
    special_char = c;
    return true;
  }
  if(!_current_input.isEmpty()) {
    return false;
  }
  // TODO handle key cmds
  //_cmd_stream->printf("key: %02X\r\n", c);
  return false;
}

void CMD_PROCESSOR::handleCrLf() {
  if(_current_input.isEmpty() && (_last_char == '\r')) {
    return;
  }
  bool found = false;
  for(auto it : _cmds) {
    if(it->handle(_cmd, _args)) {
      found = true;
      break;
    }
  }
  if(!found) {
    _cmd_stream->println(" ??");
  }
  writeHistory();
  _current_input = "";
  //_cmd_stream->printf("h:%d\r\n", _history.size());
}

void CMD_PROCESSOR::writeHistory() {
  if(!_current_input.isEmpty() && (_history.empty() || (_history.at(0) != _current_input))) {
    _history.insert(_history.begin(), _current_input);
    if(_history.size() > MAX_HISTORY) {
      _history.pop_back();
    }
  }
  _history_pos = -1;
}