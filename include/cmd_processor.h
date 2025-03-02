#ifndef CMD_PROCESSOR_H
#define CMD_PROCESSOR_H

#include <Arduino.h>
#include <vector>

class CMDS {
public:
  CMDS(String cmd, void (*callback)(String &cmd));
  CMDS(String cmd, String args, void (*callback)(String &cmd, String args, void* values));
  bool handle(String cmd, String args);
  String getCmd();
  String getArgs();
  bool operator < (const CMDS& b) const {
    return (b._cmd.length() < _cmd.length());
  }
private:
  String _cmd;
  String _args;
  void (*_callback)(String &cmd);
  void (*_callback2)(String &cmd, String args, void* values);
};

class CMD_PROCESSOR {
public:
  CMD_PROCESSOR(Stream* cmd_stream = &Serial);
  void process();
  bool registerCmd(String cmd, void (*callback)(String &cmd));
  bool registerCmd(String cmd, String args, void (*callback)(String &cmd, String args, void* values));
  bool registerKey(char key, void (*callback)(char c));
private:
  void handleCrLf();
  bool handleKey(char c);
  void writeHistory();
  void splitCmdline();
  Stream* _cmd_stream;
  String _current_input;
  uint8_t _current_pos;
  char _last_char = '\0';
  String _input_cache;
  String _cmd;
  String _args;
  std::vector<CMDS*> _cmds;
  std::vector<String> _history;
  int8_t _history_pos;
};


#endif