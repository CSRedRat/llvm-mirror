//===- Configuration.cpp - Configuration Data Mgmt --------------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the parsing of configuration files for the LLVM Compiler
// Driver (llvmc).
//
//===------------------------------------------------------------------------===

#include "Configuration.h"
#include "ConfigLexer.h"
#include "CompilerDriver.h"
#include "Config/config.h"
#include "Support/CommandLine.h"
#include "Support/StringExtras.h"
#include <iostream>
#include <fstream>

using namespace llvm;

namespace sys {
  // From CompilerDriver.cpp (for now)
  extern bool FileIsReadable(const std::string& fname);
}

namespace llvm {
  ConfigLexerInfo ConfigLexerState;
  InputProvider* ConfigLexerInput = 0;

  InputProvider::~InputProvider() {}
  void InputProvider::error(const std::string& msg) {
    std::cerr << name << ":" << ConfigLexerState.lineNum << ": Error: " << 
      msg << "\n";
    errCount++;
  }

  void InputProvider::checkErrors() {
    if (errCount > 0) {
      std::cerr << name << " had " << errCount << " errors. Terminating.\n";
      exit(errCount);
    }
  }

}

namespace {

  class FileInputProvider : public InputProvider {
    public:
      FileInputProvider(const std::string & fname)
        : InputProvider(fname) 
        , F(fname.c_str()) {
        ConfigLexerInput = this;
      }
      virtual ~FileInputProvider() { F.close(); ConfigLexerInput = 0; }
      virtual unsigned read(char *buffer, unsigned max_size) {
        if (F.good()) {
          F.read(buffer,max_size);
          if ( F.gcount() ) return F.gcount() - 1;
        }
        return 0;
      }

      bool okay() { return F.good(); }
    private:
      std::ifstream F;
  };

  cl::opt<bool> DumpTokens("dump-tokens", cl::Optional, cl::Hidden, cl::init(false),
    cl::desc("Dump lexical tokens (debug use only)."));

  struct Parser
  {
    Parser() {
      token = EOFTOK;
      provider = 0;
      confDat = 0;
      ConfigLexerState.lineNum = 1;
      ConfigLexerState.in_value = false;
      ConfigLexerState.StringVal.clear();
      ConfigLexerState.IntegerVal = 0;
    };

    ConfigLexerTokens token;
    InputProvider* provider;
    CompilerDriver::ConfigData* confDat;

    inline int next() { 
      token = Configlex();
      if (DumpTokens) 
        std::cerr << token << "\n";
      return token;
    }

    inline bool next_is_real() { 
      next();
      return (token != EOLTOK) && (token != ERRORTOK) && (token != 0);
    }

    inline void eatLineRemnant() {
      while (next_is_real()) ;
    }

    void error(const std::string& msg, bool skip = true) {
      provider->error(msg);
      if (skip)
        eatLineRemnant();
    }

    std::string parseName() {
      std::string result;
      if (next() == EQUALS) {
        while (next_is_real()) {
          switch (token ) {
            case STRING :
            case OPTION : 
              result += ConfigLexerState.StringVal + " ";
              break;
            default:
              error("Invalid name");
              break;
          }
        }
        if (result.empty())
          error("Name exepected");
        else
          result.erase(result.size()-1,1);
      } else
        error("= expected");
      return result;
    }

    bool parseBoolean() {
      bool result = true;
      if (next() == EQUALS) {
        if (next() == FALSETOK) {
          result = false;
        } else if (token != TRUETOK) {
          error("Expecting boolean value");
          return false;
        }
        if (next() != EOLTOK && token != 0) {
          error("Extraneous tokens after boolean");
        }
      }
      else
        error("Expecting '='");
      return result;
    }

    bool parseSubstitution(CompilerDriver::StringVector& optList) {
      switch (token) {
        case ARGS_SUBST:        optList.push_back("%args%"); break;
        case IN_SUBST:          optList.push_back("%in%"); break;
        case OUT_SUBST:         optList.push_back("%out%"); break;
        case TIME_SUBST:        optList.push_back("%time%"); break;
        case STATS_SUBST:       optList.push_back("%stats%"); break;
        case OPT_SUBST:         optList.push_back("%opt%"); break;
        case TARGET_SUBST:      optList.push_back("%target%"); break;
        case FORCE_SUBST:       optList.push_back("%force%"); break;
        case VERBOSE_SUBST:     optList.push_back("%verbose%"); break;
        default:
          return false;
      }
      return true;
    }

    void parseOptionList(CompilerDriver::StringVector& optList ) {
      if (next() == EQUALS) {
        while (next_is_real()) {
          if (token == STRING || token == OPTION)
            optList.push_back(ConfigLexerState.StringVal);
          else if (!parseSubstitution(optList)) {
            error("Expecting a program argument or substitution", false);
            break;
          }
        }
      } else
        error("Expecting '='");
    }

    void parseVersion() {
      if (next() == EQUALS) {
        while (next_is_real()) {
          if (token == STRING || token == OPTION)
            confDat->version = ConfigLexerState.StringVal;
          else
            error("Expecting a version string");
        }
      } else
        error("Expecting '='");
    }

    void parseLang() {
      switch (next() ) {
        case NAME: 
          confDat->langName = parseName(); 
          break;
        case OPT1: 
          parseOptionList(confDat->opts[CompilerDriver::OPT_FAST_COMPILE]); 
          break;
        case OPT2: 
          parseOptionList(confDat->opts[CompilerDriver::OPT_SIMPLE]); 
          break;
        case OPT3: 
          parseOptionList(confDat->opts[CompilerDriver::OPT_AGGRESSIVE]); 
          break;
        case OPT4: 
          parseOptionList(confDat->opts[CompilerDriver::OPT_LINK_TIME]); 
          break;
        case OPT5: 
          parseOptionList(
            confDat->opts[CompilerDriver::OPT_AGGRESSIVE_LINK_TIME]);
          break;
        default:   
          error("Expecting 'name' or 'optN' after 'lang.'"); 
          break;
      }
    }

    void parseCommand(CompilerDriver::Action& action) {
      if (next() == EQUALS) {
        if (next() == EOLTOK) {
          // no value (valid)
          action.program.clear();
          action.args.clear();
        } else {
          if (token == STRING || token == OPTION) {
            action.program.set_file(ConfigLexerState.StringVal);
          } else {
            error("Expecting a program name");
          }
          while (next_is_real()) {
            if (token == STRING || token == OPTION) {
              action.args.push_back(ConfigLexerState.StringVal);
            } else if (!parseSubstitution(action.args)) {
              error("Expecting a program argument or substitution", false);
              break;
            }
          }
        }
      }
    }

    void parsePreprocessor() {
      switch (next()) {
        case COMMAND:
          parseCommand(confDat->PreProcessor);
          break;
        case REQUIRED:
          if (parseBoolean())
            confDat->PreProcessor.set(CompilerDriver::REQUIRED_FLAG);
          else
            confDat->PreProcessor.clear(CompilerDriver::REQUIRED_FLAG);
          break;
        default:
          error("Expecting 'command' or 'required' but found '" +
              ConfigLexerState.StringVal);
          break;
      }
    }

    bool parseOutputFlag() {
      if (next() == EQUALS) {
        if (next() == ASSEMBLY) {
          return true;
        } else if (token == BYTECODE) {
          return false;
        } else {
          error("Expecting output type value");
          return false;
        }
        if (next() != EOLTOK && token != 0) {
          error("Extraneous tokens after output value");
        }
      }
      else
        error("Expecting '='");
      return false;
    }

    void parseTranslator() {
      switch (next()) {
        case COMMAND: 
          parseCommand(confDat->Translator);
          break;
        case REQUIRED:
          if (parseBoolean())
            confDat->Translator.set(CompilerDriver::REQUIRED_FLAG);
          else
            confDat->Translator.clear(CompilerDriver::REQUIRED_FLAG);
          break;
        case PREPROCESSES:
          if (parseBoolean())
            confDat->Translator.set(CompilerDriver::PREPROCESSES_FLAG);
          else 
            confDat->Translator.clear(CompilerDriver::PREPROCESSES_FLAG);
          break;
        case OUTPUT:
          if (parseOutputFlag())
            confDat->Translator.set(CompilerDriver::OUTPUT_IS_ASM_FLAG);
          else
            confDat->Translator.clear(CompilerDriver::OUTPUT_IS_ASM_FLAG);
          break;

        default:
          error("Expecting 'command', 'required', 'preprocesses', or "
                "'output' but found '" + ConfigLexerState.StringVal +
                "' instead");
          break;
      }
    }

    void parseOptimizer() {
      switch (next()) {
        case COMMAND:
          parseCommand(confDat->Optimizer);
          break;
        case PREPROCESSES:
          if (parseBoolean())
            confDat->Optimizer.set(CompilerDriver::PREPROCESSES_FLAG);
          else
            confDat->Optimizer.clear(CompilerDriver::PREPROCESSES_FLAG);
          break;
        case TRANSLATES:
          if (parseBoolean())
            confDat->Optimizer.set(CompilerDriver::TRANSLATES_FLAG);
          else
            confDat->Optimizer.clear(CompilerDriver::TRANSLATES_FLAG);
          break;
        case REQUIRED:
          if (parseBoolean())
            confDat->Optimizer.set(CompilerDriver::REQUIRED_FLAG);
          else
            confDat->Optimizer.clear(CompilerDriver::REQUIRED_FLAG);
          break;
        case OUTPUT:
          if (parseOutputFlag())
            confDat->Translator.set(CompilerDriver::OUTPUT_IS_ASM_FLAG);
          else
            confDat->Translator.clear(CompilerDriver::OUTPUT_IS_ASM_FLAG);
          break;
        default:
          error(std::string("Expecting 'command', 'preprocesses', ") +
              "'translates' or 'output' but found '" + 
              ConfigLexerState.StringVal + "' instead");
          break;
      }
    }

    void parseAssembler() {
      switch(next()) {
        case COMMAND:
          parseCommand(confDat->Assembler);
          break;
        default:
          error("Expecting 'command'");
          break;
      }
    }

    void parseLinker() {
      switch(next()) {
        case LIBS:
          break; //FIXME
        case LIBPATHS:
          break; //FIXME
        default:
          error("Expecting 'libs' or 'libpaths'");
          break;
      }
    }

    void parseAssignment() {
      switch (token) {
        case VERSION:       parseVersion(); break;
        case LANG:          parseLang(); break;
        case PREPROCESSOR:  parsePreprocessor(); break;
        case TRANSLATOR:    parseTranslator(); break;
        case OPTIMIZER:     parseOptimizer(); break;
        case ASSEMBLER:     parseAssembler(); break;
        case LINKER:        parseLinker(); break;
        case EOLTOK:        break; // just ignore
        case ERRORTOK:
        default:          
          error("Invalid top level configuration item");
          break;
      }
    }

    void parseFile() {
      while ( next() != EOFTOK ) {
        if (token == ERRORTOK)
          error("Invalid token");
        else if (token != EOLTOK)
          parseAssignment();
      }
      provider->checkErrors();
    }
  };

  void
  ParseConfigData(InputProvider& provider, CompilerDriver::ConfigData& confDat) {
    Parser p;
    p.token = EOFTOK;
    p.provider = &provider;
    p.confDat = &confDat;
    p.parseFile();
  }
}

CompilerDriver::ConfigData*
LLVMC_ConfigDataProvider::ReadConfigData(const std::string& ftype) {
  CompilerDriver::ConfigData* result = 0;
  sys::Path confFile;
  if (configDir.is_empty()) {
    // Try the environment variable
    const char* conf = getenv("LLVM_CONFIG_DIR");
    if (conf) {
      confFile.set_directory(conf);
      confFile.append_file(ftype);
      if (!confFile.readable())
        throw "Configuration file for '" + ftype + "' is not available.";
    } else {
      // Try the user's home directory
      confFile = sys::Path::GetUserHomeDirectory();
      if (!confFile.is_empty()) {
        confFile.append_directory(".llvm");
        confFile.append_directory("etc");
        confFile.append_file(ftype);
        if (!confFile.readable())
          confFile.clear();
      }
      if (!confFile.is_empty()) {
        // Okay, try the LLVM installation directory
        confFile = sys::Path::GetLLVMConfigDir();
        confFile.append_file(ftype);
        if (!confFile.readable()) {
          // Okay, try the "standard" place
          confFile = sys::Path::GetLLVMDefaultConfigDir();
          confFile.append_file(ftype);
          if (!confFile.readable()) {
            throw "Configuration file for '" + ftype + "' is not available.";
          }
        }
      }
    }
  } else {
    confFile = configDir;
    confFile.append_file(ftype);
    if (!confFile.readable())
      throw "Configuration file for '" + ftype + "' is not available.";
  }
  FileInputProvider fip( confFile.get() );
  if (!fip.okay()) {
    throw "Configuration file for '" + ftype + "' is not available.";
  }
  result = new CompilerDriver::ConfigData();
  ParseConfigData(fip,*result);
  return result;
}

LLVMC_ConfigDataProvider::~LLVMC_ConfigDataProvider()
{
  ConfigDataMap::iterator cIt = Configurations.begin();
  while (cIt != Configurations.end()) {
    CompilerDriver::ConfigData* cd = cIt->second;
    ++cIt;
    delete cd;
  }
  Configurations.clear();
}

CompilerDriver::ConfigData* 
LLVMC_ConfigDataProvider::ProvideConfigData(const std::string& filetype) {
  CompilerDriver::ConfigData* result = 0;
  if (!Configurations.empty()) {
    ConfigDataMap::iterator cIt = Configurations.find(filetype);
    if ( cIt != Configurations.end() ) {
      // We found one in the case, return it.
      result = cIt->second;
    }
  }
  if (result == 0) {
    // The configuration data doesn't exist, we have to go read it.
    result = ReadConfigData(filetype);
    // If we got one, cache it
    if (result != 0)
      Configurations.insert(std::make_pair(filetype,result));
  }
  return result; // Might return 0
}

// vim: sw=2 smartindent smarttab tw=80 autoindent expandtab
