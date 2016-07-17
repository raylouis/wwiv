/**************************************************************************/
/*                                                                        */
/*                          WWIV Version 5.x                              */
/*                Copyright (C)2016, WWIV Software Services               */
/*                                                                        */
/*    Licensed  under the  Apache License, Version  2.0 (the "License");  */
/*    you may not use this  file  except in compliance with the License.  */
/*    You may obtain a copy of the License at                             */
/*                                                                        */
/*                http://www.apache.org/licenses/LICENSE-2.0              */
/*                                                                        */
/*    Unless  required  by  applicable  law  or agreed to  in  writing,   */
/*    software  distributed  under  the  License  is  distributed on an   */
/*    "AS IS"  BASIS, WITHOUT  WARRANTIES  OR  CONDITIONS OF ANY  KIND,   */
/*    either  express  or implied.  See  the  License for  the specific   */
/*    language governing permissions and limitations under the License.   */
/**************************************************************************/

// WWIV5 Network2
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <set>
#include <string>
#include <vector>

#include "core/command_line.h"
#include "core/datafile.h"
#include "core/file.h"
#include "core/log.h"
#include "core/scope_exit.h"
#include "core/stl.h"
#include "core/strings.h"
#include "core/os.h"
#include "core/textfile.h"
#include "core/wfndfile.h"
#include "networkb/binkp.h"
#include "networkb/binkp_config.h"
#include "networkb/connection.h"
#include "networkb/net_util.h"
#include "networkb/ppp_config.h"

#include "sdk/bbslist.h"
#include "sdk/callout.h"
#include "sdk/connect.h"
#include "sdk/config.h"
#include "sdk/contact.h"
#include "sdk/datetime.h"
#include "sdk/filenames.h"
#include "sdk/networks.h"
#include "sdk/subxtr.h"
#include "sdk/usermanager.h"
#include "sdk/msgapi/msgapi.h"
#include "sdk/msgapi/message_api_wwiv.h"

using std::cout;
using std::endl;
using std::make_unique;
using std::map;
using std::set;
using std::string;
using std::unique_ptr;
using std::vector;

using namespace wwiv::core;
using namespace wwiv::net;
using namespace wwiv::os;
using namespace wwiv::sdk;
using namespace wwiv::sdk::msgapi;
using namespace wwiv::stl;
using namespace wwiv::strings;

struct Context {
  net_networks_rec* net;
  UserManager* user_manager;
  WWIVMessageApi* api;
  int network_number;
  vector<subboardrec> subs;
  vector<xtrasubsrec> xsubs;
};

static void ShowHelp(CommandLine& cmdline) {
  cout << cmdline.GetHelp()
       << ".####      Network number (as defined in INIT)" << endl
       << endl;
  exit(1);
}

static bool handle_email(Context& context,
  uint16_t to_user, const net_header_rec& nh, const string& text) {
  LOG << "handle_email to " << to_user;

  {
    User user;
    if (!context.user_manager->ReadUser(&user, to_user)) {
      // Unable to read user.
      LOG << "ERROR: Unable to read user #" << to_user << ". Discarding message.";
      return false;
    }

    if (user.IsUserDeleted()) {
      LOG << "User #" << to_user << " is deleted. Discarding message.";
      return false;
    }
  }

  EmailData d = {};
  d.daten = nh.daten;
  d.from_network_number = context.network_number;
  d.from_system = nh.fromsys;
  d.from_user = nh.fromuser;
  // All local email should have the system number set to 0.
  d.system_number = 0;
  d.user_number = nh.touser;

  auto iter = text.begin();
  d.title = get_message_field(text, iter, {'\0', '\r', '\n'}, 80);
  // Rest of the message is the text.
  d.text = string(iter, text.end());

  ScopeExit at_exit([] {
    LOG << "==============================================================";
  });
  LOG << "  Processing email.";
  LOG << "  Title: '" << d.title << "'";

  std::unique_ptr<WWIVEmail> email(context.api->OpenEmail());
  bool added = email->AddMessage(d);
  if (added) {
    User user;
    context.user_manager->ReadUser(&user, d.user_number);
    int num_waiting = user.GetNumMailWaiting();
    num_waiting++;
    user.SetNumMailWaiting(num_waiting);
    context.user_manager->WriteUser(&user, d.user_number);
    LOG << "    + Received Email  '" << d.title << "'";
  }
  return added;
}

static bool find_basename(Context& context, const string netname, string& basename) {
  int current = 0;
  for (const auto& x : context.xsubs) {
    for (const auto& n : x.nets) {
      if (n.net_num == context.network_number) {
        if (IsEqualsIgnoreCase(netname.c_str(), n.stype)) {
          // Since the subtype matches, we need to find the subboard base filename.
          // and return that.
          basename.assign(context.subs.at(current).filename);
          return true;
        }
      }
    }
    ++current;
  }
  return false;
}

// Alpha subtypes are seven characters -- the first must be a letter, but the rest can be any
// character allowed in a DOS filename.This main_type covers both subscriber - to - host and 
// host - to - subscriber messages. Minor type is always zero(since it's ignored), and the
// subtype appears as the first part of the message text, followed by a NUL.Thus, the message
// header info at the beginning of the message text is in the format 
// SUBTYPE<nul>TITLE<nul>SENDER_NAME<cr / lf>DATE_STRING<cr / lf>MESSAGE_TEXT.
static bool handle_post(Context& context, const net_header_rec& nh,
  std::vector<uint16_t>& list, const string& raw_text) {
  
  ScopeExit at_exit([] {
    LOG << "==============================================================";
  });
  auto iter = raw_text.begin();
  string subtype = get_message_field(raw_text, iter, {'\0', '\r', '\n'}, 80);
  string title = get_message_field(raw_text, iter, {'\0', '\r', '\n'}, 80);
  string sender_name = get_message_field(raw_text, iter, {'\0', '\r', '\n'}, 80);
  string date_string = get_message_field(raw_text, iter, {'\0', '\r', '\n'}, 80);
  string text = string(iter, raw_text.end());
  LOG << "==============================================================";
  LOG << "  Processing New Post on subtype: " << subtype;
  LOG << "  title:   " << title;
  LOG << "  sender:  " << sender_name;
  LOG << "  date:    " << date_string;

  string basename;
  if (!find_basename(context, subtype, basename)) {
    LOG << "ERROR: Unable to find subtype of subtype: " << subtype;
    return false;
  }

  if (!context.api->Exist(basename)) {
    LOG << "WARNING Message area: '" << basename << "' does not exist.";;
    LOG << "WARNING Attempting to create it.";
    // Since the area does not exist, let's create it automatically
    // like WWIV alwyas does.
    unique_ptr<MessageArea> creator(context.api->Create(basename));
    if (!creator) {
      LOG << "ERROR: Failed to create message area: " << basename << ". Exiting.";
      return false;
    }
  }

  unique_ptr<MessageArea> area(context.api->Open(basename));
  if (!area) {
    LOG << "ERROR Unable to open message area: '" << basename << "'.";
    return false;
  }

  unique_ptr<Message> msg(area->CreateMessage());
  msg->header()->set_from_system(nh.fromsys);
  msg->header()->set_from_usernum(nh.fromuser);
  msg->header()->set_title(title);
  msg->header()->set_from(sender_name);
  msg->header()->set_daten(nh.daten);
  msg->text()->set_text(text);

  const int num_messages = area->number_of_messages();
  for (int current = 1; current <= num_messages; current++) {
    unique_ptr<MessageHeader> header(area->ReadMessageHeader(current));
    if (!header) {
      continue;
    }

    // Since we don't have a global message id, use the combination of
    // date + title + from system + from user.
    if (header->daten() == nh.daten 
      && header->title() == title
      && header->from_system() == nh.fromsys
      && header->from_usernum() == nh.fromuser) {
      LOG << "  Discarding Duplicate Message.";
      return false;
    }
  }

  bool result = area->AddMessage(*msg);
  if (!result) {
    LOG << "  Failed to add message: " << title;
    return false;
  }
  LOG << "    + Posted  '" << title << "'";
  return true;
}

static string NetInfoFileName(uint16_t type) {
  switch (type) {
  case net_info_bbslist: return BBSLIST_NET;
  case net_info_connect: return CONNECT_NET;
  case net_info_sub_lst: return SUBS_LST;
  case net_info_wwivnews: return "wwivnews.net";
  case net_info_more_wwivnews: return "wwivnews.net";
  case net_info_categ_net: return CATEG_NET;
  case net_info_network_lst: return "networks.lst";
  case net_info_file: return "";
  case net_info_binkp: return BINKP_NET;
  }
  return "";
}

static bool handle_net_info_file(const net_networks_rec& net,
  const net_header_rec& nh, const string& text) {

  string filename = NetInfoFileName(nh.minor_type);
  if (nh.minor_type == net_info_file) {
    // we don't know the filename
    LOG << "ERROR: net_info_file not supported.";
    return false;
  } else if (!filename.empty()) {
    // we know the name.
    File file(net.dir, filename);
    if (!file.Open(File::modeWriteOnly | File::modeBinary | File::modeCreateFile | File::modeTruncate, 
      File::shareDenyReadWrite)) {
      // We couldn't create or open the file.
      LOG << "ERROR: Unable to create or open file: " << filename;
      return false;
    }
    file.Write(text);
    LOG << "  + Got " << filename;
    return true;
  }
  // error.
  return false;
}

static bool handle_packet(
  Context& context,
  const net_header_rec& nh, std::vector<uint16_t>& list, const string& text) {
  LOG << "Processing message with type: " << main_type_name(nh.main_type)
      << "/" << nh.minor_type;

  switch (nh.main_type) {
    /*
    These messages contain various network information
    files, encoded with method 1 (requiring DE1.EXE).
    Once DE1.EXE has verified the source and returned to
    the analyzer, the file is created in the network's
    DATA directory with the filename determined by the
    minor_type (except minor_type 1).
    */
  case main_type_net_info:
    if (nh.minor_type == 0) {
      // Feedback to sysop from the NC.  
      // This is sent to the #1 account as source verified email.
      return handle_email(context, 1, nh, text);
    } else {
      return handle_net_info_file(*context.net, nh, text);
    }
  break;
  case main_type_email:
    // This is regular email sent to a user number at this system.
    // Email has no minor type, so minor_type will always be zero.
    return handle_email(context, nh.touser, nh, text);
  break;
  case main_type_new_post:
  {
    return handle_post(context, nh, list, text);
  } break;
  // Legacy numeric only post types.
  case main_type_pre_post:
  case main_type_post:
  default:
    LOG << "Writing message to dead.net for unhandled type: " << main_type_name(nh.main_type);
    return write_packet(DEAD_NET, *context.net, nh, list, text);
  }

  return false;
}

static bool handle_file(Context& context, const string& name) {
  File f(context.net->dir, name);
  if (!f.Open(File::modeBinary | File::modeReadOnly)) {
    LOG << "Unable to open file: " << context.net->dir << name;
    return false;
  }

  bool done = false;
  while (!done) {
    net_header_rec nh;
    string text;
    int num_read = f.Read(&nh, sizeof(net_header_rec));
    if (num_read == 0) {
      // at the end of the packet.
      return true;
    }
    if (num_read != sizeof(net_header_rec)) {
      LOG << "error reading header, got short read of size: " << num_read
        << "; expected: " << sizeof(net_header_rec);
      return false;
    }
    if (nh.method > 0) {
      LOG << "compression: de" << nh.method;
    }

    std::vector<uint16_t> list;
    if (nh.list_len > 0) {
      // skip list of addresses.
      list.resize(nh.list_len);
      f.Read(&list[0], 2 * nh.list_len);
    }
    if (nh.length > 0) {
      int length = nh.length;
      if (nh.method > 0) {
        length -= 146; // sizeof EN/DE header.
        // HACK - this should do this in a shim DE
        nh.length -= 146; 
        char header[147];
        f.Read(header, 146);
      }
      text.resize(length);
      f.Read(&text[0], length);
    }
    if (!handle_packet(context, nh, list, text)) {
      LOG << "Error handing packet: type: " << nh.main_type;
    }
  }
  return true;
}

static vector<subboardrec> read_subs(const string &datadir) {
  std::vector<subboardrec> subboards;

  DataFile<subboardrec> file(datadir, SUBS_DAT);
  if (!file) {
    std::clog << file.file().GetName() << " NOT FOUND." << std::endl;
    return {};
  }
  if (!file.ReadVector(subboards)) {
    return {};
  }
  return subboards;
}

int main(int argc, char** argv) {
  Logger::Init(argc, argv);
  try {
    ScopeExit at_exit(Logger::ExitLogger);
    CommandLine cmdline(argc, argv, "network_number");
    cmdline.set_no_args_allowed(true);
    cmdline.add_argument({"network", "Network name to use (i.e. wwivnet).", ""});
    cmdline.add_argument({"network_number", "Network number to use (i.e. 0).", "0"});
    cmdline.add_argument({"bbsdir", "(optional) BBS directory if other than current directory", File::current_directory()});
    cmdline.add_argument(BooleanCommandLineArgument("help", '?', "displays help.", false));
    cmdline.add_argument(BooleanCommandLineArgument("feedback", 'y', "Sends feedback.", false));

    if (!cmdline.Parse() || cmdline.arg("help").as_bool()) {
      ShowHelp(cmdline);
      return 2;
    }
    string network_name = cmdline.arg("network").as_string();
    string network_number = cmdline.arg("network_number").as_string();
    if (network_name.empty() && network_number.empty()) {
      LOG << "--network=[network name] or .[network_number] must be specified.";
      ShowHelp(cmdline);
      return 2;
    }

    string bbsdir = cmdline.arg("bbsdir").as_string();
    Config config(bbsdir);
    if (!config.IsInitialized()) {
      LOG << "Unable to load CONFIG.DAT.";
      return 3;
    }
    Networks networks(config);
    if (!networks.IsInitialized()) {
      LOG << "Unable to load networks.";
      return 4;
    }

    int network_number_int = 0;
    if (!network_number.empty() && network_name.empty()) {
      // Need to set the network name based on the number.
      network_number_int = std::stoi(network_number);
      network_name = networks[network_number_int].name;
    }
    if (network_number.empty() && !network_name.empty()) {
      int num = 0;
      for (const auto& n : networks.networks()) {
        if (network_name == n.name) {
          network_number_int = num;
        }
      }
    }

    LOG << "NETWORK2 for network: " << network_name;
    auto net = networks[network_name];

    if (!File::Exists(net.dir, LOCAL_NET)) {
      LOG << "No local.net exists. exiting.";
      return 0;
    }

    unique_ptr<WWIVMessageApi> api = make_unique<WWIVMessageApi>(
      bbsdir, config.datadir(), config.msgsdir(), networks.networks());
    unique_ptr<UserManager> user_manager = make_unique<UserManager>(
      config.config()->datadir, config.config()->userreclen, config.config()->maxusers);
    Context context;
    context.net = &net;
    context.user_manager = user_manager.get();
    context.network_number = network_number_int;
    context.api = api.get();
    context.subs = std::move(read_subs(config.datadir()));
    if (!read_subs_xtr(config.datadir(), networks.networks(), context.subs, context.xsubs)) {
      LOG << "ERROR: Failed to read file: " << SUBS_XTR;
      return 5;
    }

    LOG << "Processing: " << net.dir << LOCAL_NET;
    if (handle_file(context, LOCAL_NET)) {
      LOG << "Deleting: " << net.dir << LOCAL_NET;
      File::Remove(net.dir, LOCAL_NET);
      return 0;
    } else {
      LOG << "ERROR: handle_file returned false";
    }
    return 1;
  } catch (const std::exception& e) {
    LOG << "ERROR: [network]: " << e.what();
  }

  return 255;
}