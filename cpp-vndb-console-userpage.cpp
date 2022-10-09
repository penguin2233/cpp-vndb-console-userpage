#include <iostream>
#include <string>
#include <thread>
#include <chrono>
#include <mutex>
#include <fstream>
#include <boost/exception/all.hpp>
#include <boost/bind/bind.hpp>
#include <boost/asio.hpp>

#ifdef USING_SSL
#include <boost/asio/ssl.hpp>
#endif

// Copyright (c) 2022 https://penguin2233.gq, no warranty is provided

const std::string CLIENT_NAME = "cpp-vndb-console-userpage";
const std::string CLIENT_VER = "MZ-E620-win";

#ifdef USING_SSL
boost::asio::io_context io_context;
boost::asio::ssl::context sslcontext(boost::asio::ssl::context::sslv23);
boost::asio::ssl::stream<boost::asio::ip::tcp::socket> mainsocket(io_context, sslcontext);
#else
boost::asio::io_service io_man;
boost::asio::ip::tcp::socket mainsocket(io_man);
#endif
boost::system::error_code error;

const char eofbyte = 0x04; // funny anidb EOT signal
const char* eofbyteX = reinterpret_cast<const char*>(&eofbyte);

std::mutex toParseMtx;
std::string toParse;

struct VN { int uid; int vn; int added; int lastmod; int voted; int vote; std::string notes; std::string started; std::string finished; };

std::vector<std::string> VNsRaw;
std::vector<VN> VNs;

bool exitSignal = false;
bool direct = false;

bool ok = false;
std::condition_variable ok_cv;
std::mutex ok_cv_m;

std::string uid;

std::string fetchString(std::string request) {
	std::string response;
	boost::asio::write(mainsocket, boost::asio::buffer(request), error);
	boost::asio::write(mainsocket, boost::asio::buffer(eofbyteX, sizeof(char)), error);
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	while (true) {
		toParseMtx.lock();
		if (toParse.empty()) {
			toParseMtx.unlock();
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		else if (!toParse.empty()) {
			if (toParse[toParse.size() - 1] == 0x04) {
				response = toParse;
				toParse.clear();
				toParseMtx.unlock();
				break;
			}
		}
	}
	return response;
}

std::vector<std::string> parseArray(std::string searchTerm, std::string raw, bool arrayOfObjects, bool removeQuotes, bool isolate) {
	std::vector<std::string> ret;
	size_t arrayStart;
	size_t arrayEnd;
	bool listEnd = false;

	if (!arrayOfObjects) arrayStart = raw.find("\"" + searchTerm + "\":[");
	else if (arrayOfObjects) arrayStart = raw.find("\"" + searchTerm + "\":{");
	if (arrayStart != std::string::npos) {
		std::string secondPass = raw.substr(arrayStart + 1 + searchTerm.size() + 3, raw.size());
		if (!arrayOfObjects) arrayEnd = secondPass.find("],\"");
		else if (arrayOfObjects) arrayEnd = secondPass.find("},\"");
		secondPass = secondPass.substr(0, arrayEnd);
		while (!listEnd) {
			size_t itemEnd = secondPass.find(",\"");
			if (itemEnd != std::string::npos) {
				ret.push_back(secondPass.substr(0, itemEnd));
				secondPass.erase(0, itemEnd + 1);
			}
			else listEnd = true;
		}
		// get last item in array
		ret.push_back(secondPass.substr(0, arrayEnd));
	}
	else return ret; // return empty

	if (ret.size() == 1 && (ret[0] == "null" || ret[0] == "\"null\"") && !isolate) {
		ret.clear();
		return ret;
	}

	if (removeQuotes) {
		for (size_t i = 0; i < ret.size(); i++) {
			if (ret[i].size() > 3) {
				if (ret[i].size() > 2 && ret[i][ret[i].size() - 1] == ']' || ret[i].size() > 2 && ret[i][ret[i].size() - 2] == '}' || ret[i].size() > 2 && ret[i][ret[i].size() - 2] == ']') {
					if (ret[i][ret[i].size() - 2] == '}') {
						if (ret[i][ret[i].size() - 1] == 0x04) ret[i].erase(ret[i].size() - 5, ret[i].size());
						else if (ret[i][ret[i].size() - 3] == ']')  ret[i].erase(ret[i].size() - 4, ret[i].size());
						else ret[i].erase(ret[i].size() - 2, ret[i].size());
					}
					if (ret[i][ret[i].size() - 2] == ']') {
						ret[i].erase(ret[i].size() - 2, ret[i].size());
					}
				}
			}
			if (ret[i].size() > 2 && ret[i][0] == '\"' && ret[i][ret[i].size() - 1] == '\"') {
				ret[i] = ret[i].substr(1, ret[i].size() - 2);
			}
		}
	}
	else if (!removeQuotes) {
		for (size_t i = 0; i < ret.size(); i++) {
			if (ret[i][ret[i].size() - 1] == ']' || ret[i][ret[i].size() - 1] == '}' || ret[i][ret[i].size() - 2] == '}') {
				if (ret[i][ret[i].size() - 2] == '}') {
					if (ret[i][ret[i].size() - 3] == ']')  ret[i].erase(ret[i].size() - 4, ret[i].size());
					else ret[i].erase(ret[i].size() - 2, ret[i].size());
				}
				if (ret[i][ret[i].size() - 2] == ']') {
					ret[i].erase(ret[i].size() - 3, ret[i].size());
				}
			}
		}
	}

	if (isolate) { // give back raw as last element of vector but without array we just parsed
		raw.erase(arrayStart, 1 + searchTerm.size() + 3 + arrayEnd + 2);
		ret.push_back(raw);
	}

	return ret;
}

std::string parseString(std::string searchTerm, std::string raw, bool integer, bool removeQuotes) {
	std::string ret;
	size_t location = raw.find("\"" + searchTerm + "\":");
	//if (searchTerm == "voted") { // special case for vote and voted 
	//	if (raw[location + 5] == 'd') ; else {
	//		raw.erase(0, location + 5);
	//		size_t location = raw.find("\"" + searchTerm + "\":");
	//	}
	//}
	if (location != std::string::npos) {
		std::string secondPass = raw.substr(location + 1 + searchTerm.size() + 2, raw.size());
		size_t end = secondPass.find(",\"");
		std::string value = secondPass.substr(0, end);
		if (value == "null" || value == "null}]" || value == "null}" || value == "\"\"") {
			if (integer == true) return "0"; else return "";
		}
		else ret = value;
		if (value[0] == '\"' && removeQuotes) {
			value = value.substr(1, value.size() - 2);
			if (value[value.size() - 1] == '}') value = value.substr(0, value.size() - 2);
			if (value.size() > 2 && value[value.size() - 1] == ']' || value.size() > 2 && value[value.size() - 1] == '}') {
				if (value.size() > 10 && value[value.size() - 5] != 'u' && value[value.size() - 4] != 'r' && value[value.size() - 3] != 'l') {
					if (value[value.size() - 2] == ']') {
						value.erase(value.size() - 3, value.size());
					}
					if (value[value.size() - 2] == '}') {
						if (value[value.size() - 3] == ']')  value.erase(value.size() - 4, value.size());
						else value.erase(value.size() - 2, value.size());
					}
				}

				/*if (value[value.size() - 2] == '}') {
					if (value[value.size() - 3] == ']')  value.erase(value.size() - 4, value.size());
					else value.erase(value.size() - 2, value.size());
				}*/
			}
			ret = value;
		}
		if (value.size() > 3 && !removeQuotes) {
			if (value[value.size() - 1] == ']' || value[value.size() - 2] == '}') {
				if (value[value.size() - 2] == '}') {
					if (value[value.size() - 3] == ']')  value.erase(value.size() - 4, value.size());
					else value.erase(value.size() - 2, value.size());
				}
				else if (value[value.size() - 2] == ']') {
					value.erase(value.size() - 2, value.size());
				}
			}
			if (value == "null") {
				if (integer == true) return "0"; 
				else return "";
			}
			ret = value;
		}
	}
	else if (integer) ret = "0"; else ret = "";
	return ret;
}

void parseVNs() {
	VNs.clear();
	for (size_t i = 0; i < VNsRaw.size(); i++) {
		struct VN h = {
			stoi(parseString("uid", VNsRaw[i], true, false)),
			stoi(parseString("vn", VNsRaw[i], true, false)),
			stoi(parseString("added", VNsRaw[i], true, false)),
			stoi(parseString("lastmod", VNsRaw[i], true, false)),
			stoi(parseString("voted", VNsRaw[i], true, false)),
			stoi(parseString("vote", VNsRaw[i], true, false)) ,
			parseString("notes", VNsRaw[i], false, true),
			parseString("started", VNsRaw[i], false, false),
			parseString("finished", VNsRaw[i], false, false),
		};
		VNs.push_back(h);
	}
	return;
}

void userPage() {
	for (int i = static_cast<int>(VNs.size() - 1); i >= 0; i--) { // display VNs in reverse order with latest modified at top
		// auto t1 = std::chrono::high_resolution_clock::now();
		std::string parse = fetchString("get vn basic (id = " + std::to_string(VNs[i].vn) + ')');
		/*auto t2 = std::chrono::high_resolution_clock::now();
		auto EXTime = std::chrono::duration_cast<std::chrono::milliseconds>(t2 - t1);
		std::cout << "EXECUTION TIME: " << EXTime.count() << '\n';*/

		std::cout << "VN ID: " << VNs[i].vn << "   ";

		std::string title = parseString("title", parse, false, false);
		std::string original = parseString("original", parse, false, false);
		std::cout << "Title: " << title << "   ";
		if (original != "") std::cout << "Original Name: " << original << "   ";

		std::cout << '\n';
		if (VNs[i].notes != "") std::cout << "Notes: " << VNs[i].notes << '\n';
		if (VNs[i].vote != 0) std::cout << "Vote: " << VNs[i].vote;

		std::cout << "\n\n";
	}
	return;
}

void lastmod10() {
	std::string parse = fetchString("get ulist basic (uid = " + uid + ") {\"results\":10,\"sort\":\"lastmod\"}");

	int num = stoi(parseString("num", parse, true, false));
	if (num == 0) {
		std::cout << "No visual novels to display. \n";
		return;
	}

	std::size_t foundListStart = parse.find("[{");
	parse = parse.substr(foundListStart + 2, parse.size());

	for (int i = 0; i < num; i++) {
		std::size_t foundEnd = parse.find("},");
		if (foundEnd != std::string::npos) {
			VNsRaw.push_back(parse.substr(0, foundEnd));
			parse.erase(0, foundEnd + 2);
		}
	}

	// get last in the list
	std::size_t foundEnd = parse.find("}]");
	if (foundEnd != std::string::npos) {
		VNsRaw.push_back(parse.substr(0, foundEnd));
		parse.erase(0, foundEnd + 2);
	}

	parseVNs();
	VNsRaw.clear();

	std::cout << "Displaying last modified " << num << " VNs. \n\n";

	// auto t1 = std::chrono::high_resolution_clock::now();

	userPage();

	/*auto t2 = std::chrono::high_resolution_clock::now();
	auto EXTime = std::chrono::duration_cast<std::chrono::seconds>(t2 - t1);
	std::cout << "EXECUTION TIME FOR USERPAGE: " << EXTime.count() << '\n';*/

	return;
}

void displayByLabel(std::string labelName, std::string labelID) {
	std::string parse = fetchString("get ulist basic (uid = " + uid + " and label = " + labelID + ") {\"results\":10,\"sort\":\"lastmod\"}");

	int num = stoi(parseString("num", parse, true, false));
	if (num == 0) {
		std::cout << "No visual novels to display. \n\n";
		return;
	}

	std::size_t foundListStart = parse.find("[{");
	parse = parse.substr(foundListStart + 2, parse.size());

	for (int i = 0; i < num; i++) {
		std::size_t foundEnd = parse.find("},");
		if (foundEnd != std::string::npos) {
			VNsRaw.push_back(parse.substr(0, foundEnd));
			parse.erase(0, foundEnd + 2);
		}
	}

	// get last in the list
	std::size_t foundEnd = parse.find("}]");
	if (foundEnd != std::string::npos) {
		VNsRaw.push_back(parse.substr(0, foundEnd));
		parse.erase(0, foundEnd + 2);
	}

	parseVNs();
	VNsRaw.clear();

	std::cout << "Displaying " << num << " VNs with label \"" << labelName << "\". \n\n";

	userPage();

	return;
}

void lookupUser() {
	std::string tempUID;
	bool needUID = true;
	std::cout << '\n';
	bool ask = true;
	std::string input;
	while (ask) {
		std::cout << "What do you want to look up? (user/user-labels): ";
		std::cin >> input;

		if (input == "user") {
			std::string output;
			bool lookupUsername = false;
			if (!uid.empty()) {
				char selfUserLookup;
				std::cout << "Look up your own username? (Y/N): ";
				std::cin >> selfUserLookup;
				while (ask) {
					switch (selfUserLookup) {
					case 'Y': { input = uid; ask = false; needUID = false; lookupUsername = true; break; }
					case 'y': { input = uid; ask = false; needUID = false; lookupUsername = true; break; }
					case 'N': { ask = false; break; }
					case 'n': { ask = false; break; }
					default: { std::cout << "Invalid choice. \n"; }
					}
				}
			}

			if (needUID) {
				std::cout << "Who do you want to look up (username or UID)?: ";
				std::cin >> input;
				try {
					stoi(input);
					lookupUsername = true;
				}
				catch (std::invalid_argument) {
					lookupUsername = false;
				}
			}

			std::cout << '\n';

			if (lookupUsername) {
				std::string response = fetchString("get user basic (id = " + input + ')');
				output = parseString("username", response, false, false);
				std::cout << "Username of UID" << input << ": " << output << '\n';
			}
			else {
				std::string response = fetchString("get user basic (username = \"" + input + "\")");
				output = parseString("id", response, true, false);
				std::cout << "UID of Username " << input << ": " << output << '\n';
			}

			ask = false;
		}
		else if (input == "user-labels") {
			if (!uid.empty()) {
				char selfLabelLookup;
				std::cout << "Look up your own labels? (Y/N): ";
				std::cin >> selfLabelLookup;
				while (ask) {
					switch (selfLabelLookup) {
					case 'Y': { tempUID = uid; ask = false; needUID = false; break; }
					case 'y': { tempUID = uid; ask = false; needUID = false; break; }
					case 'N': { ask = false; break; }
					case 'n': { ask = false; break; }
					default: { std::cout << "Invalid choice. \n"; }
					}
				}
			}
			if (needUID) {
				std::cout << "Who do you want to look up (username or UID)?: ";
				std::cin >> input;
				try {
					stoi(input);
				}
				catch (std::invalid_argument) {
					std::string response = fetchString("get user basic (username = \"" + input + "\")");
					tempUID = parseString("id", response, true, false);
				}
				ask = false;
			}

			std::cout << '\n';
			std::cout << "Reminder: this program will not fetch private labels. \n\n";

			std::string parse = fetchString("get ulist-labels basic (uid = " + tempUID + ")");
			int num = stoi(parseString("num", parse, true, false));
			if (num == 0) {
				std::cout << "No user labels to display. \n";
				return;
			}

			std::cout << '\n';

			std::size_t foundListStart = parse.find("[{");
			parse = parse.substr(foundListStart + 2, parse.size());

			std::vector<std::string> userLabelsRaw;
			struct userLabel { std::string userLabelName; int userLabelID; };
			std::vector<userLabel> labels;

			for (int i = 0; i < num; i++) {
				std::size_t foundEnd = parse.find("},");
				if (foundEnd != std::string::npos) {
					userLabelsRaw.push_back(parse.substr(0, foundEnd));
					parse.erase(0, foundEnd + 2);
				}
			}

			// get last in the list
			std::size_t foundEnd = parse.find("}]");
			if (foundEnd != std::string::npos) {
				userLabelsRaw.push_back(parse.substr(0, foundEnd));
				parse.erase(0, foundEnd + 2);
			}

			for (size_t i = 0; i < userLabelsRaw.size(); i++) {
				std::string labelName = parseString("label", userLabelsRaw[i], false, false);
				int labelID = stoi(parseString("id", userLabelsRaw[i], true, false));
				struct userLabel h = { labelName, labelID };
				labels.push_back(h);
			}

			std::cout << "Displaying " << num << " labels. \n";
			for (size_t i = 0; i < labels.size(); i++) {
				std::cout << "Label ID: " << labels[i].userLabelID << "  Label name:" << labels[i].userLabelName << '\n';
			}
		}
		else {
			std::cout << "Invalid input. \n";
		}
	}
	std::cout << '\n';
	std::cin.clear();
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	return;
}

void lookupVN() {
	bool ask = true;
	std::string input;
	std::string vnid;
	std::cout << '\n';
	std::string parse;
	std::string title;
	while (ask) {
		std::cout << "Lookup by ID or Title? (id/title): ";
		std::cin >> input;
		if (input == "id") {
			std::cout << "\nVNID: ";
			std::cin >> vnid;
			std::cin.ignore();
			try {
				stoi(vnid);
				ask = false;
			}
			catch (std::invalid_argument) {
				std::cout << "\nInvalid input. \n";
				continue;
			}
			catch (std::out_of_range) {
				std::cout << "\nInvalid input. \n";
				continue;
			}
			parse = fetchString("get vn basic,details,stats (id =  " + vnid + ")");
		}
		else if (input == "title") {
			std::cout << "\nTitle: ";
			std::cin >> std::ws;
			getline(std::cin, title);
			ask = false;
			parse = fetchString("get vn basic,details,stats (title =  \"" + title + "\")");
		}
		else std::cout << "\nInvalid input. \n";
	}

	int matches = 0;
	for (int i = 0; i <= 5; i++) {
		if (parse[i] == "error"[i]) matches++;
	}
	if (matches == (5)) {
		std::string error = parseString("msg", parse, false, false);
		if (error == "\"Invalid identifier\"") {
			std::cout << parse;
			return;
		}
	}
	int num = stoi(parseString("num", parse, true, false));
	if (num == 0) {
		std::cout << "No results found. \n";
		return;
	}

	// basic
	vnid = parseString("id", parse, true, false);
	title = parseString("title", parse, false, false);
	std::string original = parseString("original", parse, false, false);
	std::string released = parseString("released", parse, false, true);
	std::vector<std::string> languages = parseArray("languages", parse, false, true, false);
	std::vector<std::string> originalLanguages = parseArray("orig_lang", parse, false, true, false);
	std::vector<std::string> platforms = parseArray("platforms", parse, false, true, false);

	// details
	int length = stoi(parseString("length", parse, true, false));
	std::string description = parseString("description", parse, false, true);

	// details - aliases
	std::vector<std::string> aliases;
	std::string aliasesRaw = parseString("aliases", parse, false, true);
	if (aliasesRaw != "" && aliasesRaw != "\"null}\"") {
		bool listEnd = false;
		while (!listEnd) {
			size_t location = aliasesRaw.find("\\n");
			if (location != std::string::npos) {
				aliases.push_back('\"' + aliasesRaw.substr(0, location) + '\"');
				aliasesRaw.erase(0, location + 2);
			}
			else listEnd = true;
		}
		// if (aliasesRaw[aliasesRaw.size() - 2] == '}') aliasesRaw = aliasesRaw.substr(0, aliasesRaw.size() - 3);
		if (aliasesRaw[aliasesRaw.size() - 1] == '}') aliasesRaw = aliasesRaw.substr(0, aliasesRaw.size() - 2);
		// if (aliasesRaw[aliasesRaw.size() - 1] == '}') aliasesRaw = aliasesRaw.substr(0, aliasesRaw.size() - 2);
		aliases.push_back('\"' + aliasesRaw + '\"');
		if (aliases[aliases.size() - 1][aliases[aliases.size() - 1].size() - 1] == '\"'
			&& aliases[aliases.size() - 1][aliases[aliases.size() - 1].size() - 2] == '}'
			&& aliases[aliases.size() - 1][aliases[aliases.size() - 1].size() - 3] == ']'
			&& aliases[aliases.size() - 1][aliases[aliases.size() - 1].size() - 4] == '\"') {
			aliases[aliases.size() - 1] = aliases[aliases.size() - 1].substr(0, aliases.size() - 4);
		}
	}

	// stats
	float popularity = stof(parseString("popularity", parse, true, false));
	float rating = stof(parseString("rating", parse, true, false));
	int votecount = stoi(parseString("votecount", parse, true, false));

	// display
	std::cout << "\n\n";
	std::cout << "VNID: " << vnid << "   ";
	std::cout << "Title: " << title << "   ";
	if (original != "") std::cout << "Original Name: " << original << "\n\n"; else std::cout << "\n\n";
	if (released != "") std::cout << "Released: " << released << '\n'; else std::cout << '\n';
	std::cout << "Original Language(s): ";
	for (size_t i = 0; i < originalLanguages.size(); i++) {
		std::cout << originalLanguages[i] << ' ';
	}
	if (!languages.empty()) {
		std::cout << "\nLanguages: ";
		for (size_t i = 0; i < languages.size(); i++) {
			std::cout << languages[i] << ' ';
		}
	}
	else std::cout << '\n';
	if (!platforms.empty()) {
		std::cout << "\nPlatforms: ";
		for (size_t i = 0; i < platforms.size(); i++) {
			std::cout << platforms[i] << ' ';
		}
	}
	else std::cout << '\n';
	if (!aliases.empty()) {
		std::cout << "\nAliases: ";
		for (size_t i = 0; i < aliases.size(); i++) {
			std::cout << aliases[i] << "  ";
		}
		std::cout << '\n';
	}
	else std::cout << '\n';
	if (length != 0) std::cout << "Game Length: " << length << "\n\n"; else std::cout << '\n';
	if (description != "") {
		for (size_t i = 0; i < description.size(); i++) {
			if (description[i] != '\\') std::cout << description[i];
			else if (description[i + 1] == 'n') { // new line when \n
				std::cout << '\n';
				i++;
			}
		}
		std::cout << "\n\n";
	}
	else std::cout << '\n';
	std::cout << "Popularity: " << popularity << '\n';
	std::cout << "Rating: " << rating << '\n';
	std::cout << "Votecount: " << votecount << '\n';
	std::cout << "\nLink to VN on VNDB: https://vndb.org/v" << vnid << "\n\n";
	return;
}

void lookup() { // not fully implemented yet
	std::cout << '\n';
	bool ask = true;
	std::string input;
	while (ask) {
		std::cout << "What do you want to look up? (release/character/producer/staff): ";
		std::cin >> input;
		if (input == "release") { ask = false; lookupVN(); }
		else if (input == "character") { ask = false; lookupVN(); }
		else if (input == "producer") { ask = false; lookupVN(); }
		else if (input == "staff") { ask = false; lookupVN(); }
		else { std::cout << "\nInvalid input. \n"; }
	}
	std::cin.clear();
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	std::cout << '\n';
	return;
}

void dbstats() {
	std::cout << '\n';

	std::string parse = fetchString("dbstats");

	int tags = stoi(parseString("tags", parse, true, false));
	int releases = stoi(parseString("releases", parse, true, false));
	int producers = stoi(parseString("producers", parse, true, false));
	int chars = stoi(parseString("chars", parse, true, false));
	int vn = stoi(parseString("vn", parse, true, false));
	int traits = stoi(parseString("traits", parse, true, false));
	int staff = stoi(parseString("staff", parse, true, false));

	std::cout
		<< "Visual Novels: " << vn << '\n'
		<< "Releases: " << releases << '\n'
		<< "Characters: " << chars << '\n'
		<< "Tags: " << tags << '\n'
		<< "Traits: " << traits << '\n'
		<< "Producers: " << producers << '\n'
		<< "Staff: " << staff << "\n\n";

	return;
}

void quote() {
	std::cout << '\n';
	std::string parse = fetchString("get quote basic (id >= 1) {\"results\": 1}");
	int vnid = stoi(parseString("id", parse, true, false));
	std::string quote = parseString("quote", parse, false, false);
	std::string title = parseString("title", parse, true, false);
	std::cout << quote << '\n' << "From: " << title << " VN ID: " << vnid << "\n\n";
	return;
}

void connect() {
#ifdef USING_SSL
	// set up ssl
	sslcontext.set_default_verify_paths();
	boost::asio::ip::tcp::resolver resolver(io_context);
	boost::asio::ip::tcp::resolver::query query("api.vndb.org", "19535"); // TCP yes TLS
	try { // connect with SSL
		boost::asio::connect(mainsocket.lowest_layer(), resolver.resolve(query));
		mainsocket.lowest_layer().set_option(boost::asio::ip::tcp::no_delay(true));
		mainsocket.set_verify_mode(boost::asio::ssl::verify_peer);
		mainsocket.set_verify_callback(boost::asio::ssl::host_name_verification("vndb.org"));
		mainsocket.handshake(boost::asio::ssl::stream<boost::asio::ip::tcp::socket>::client);
	}
	catch (boost::exception& e) {
		std::cout << "Exception thrown trying to connect with SSL. " << boost::diagnostic_information(e) << '\n';
		return;
	}
	std::cout << "Socket opened (SSL). \n";
	std::cout << "Logging in... \n\n";
#else
	// open socket to vndb server
	boost::asio::ip::tcp::resolver resolver(io_man);
	boost::asio::ip::tcp::resolver::query query("api.vndb.org", "19534"); // TCP no TLS
	boost::asio::ip::tcp::resolver::iterator iterator = resolver.resolve(query);
	boost::asio::ip::tcp::endpoint endpoint = iterator->endpoint();
	try {
		mainsocket.connect(endpoint);
	}
	catch (boost::exception& e) {
		std::cout << "Exception thrown trying to connect. " << boost::diagnostic_information(e) << '\n';
		return;
	}
	std::cout << "Socket opened. \n";
	std::cout << "Logging in... \n\n";
#endif
	// login
	std::string credentials = "login{ \"protocol\":1,\"client\":\"" + CLIENT_NAME + "\",\"clientver\":\"" + CLIENT_VER + "\"}";
	boost::asio::write(mainsocket, boost::asio::buffer(credentials), error);
	boost::asio::write(mainsocket, boost::asio::buffer(eofbyteX, sizeof(char)), error);
	return;
}

void write() {
	direct = true;
	while (!exitSignal)
	{
		std::string writebuf;
		getline(std::cin, writebuf);
		if (writebuf == "menu") {
			direct = false;
			return;
		}
		else { // direct send to vndb server
			boost::asio::write(mainsocket, boost::asio::buffer(writebuf), error);
			boost::asio::write(mainsocket, boost::asio::buffer(eofbyteX, sizeof(char)), error);
		}
	}
}

void read() {
	connect();
	std::string buffer;
	while (!exitSignal) {
		// char buffer[1];
		try {
			size_t len = boost::asio::read_until(mainsocket, boost::asio::dynamic_buffer(buffer), "");
			// mainsocket.receive(boost::asio::buffer(buffer, sizeof(buffer)));
			// std::cout.write(buffer, sizeof(buffer));
			// std::cout.write(buffer.c_str(), len);
			toParseMtx.lock();
			// toParse.push_back(buffer[0]);
			if (!ok) {
				if (buffer == ("ok")) {
					ok = true;
					std::cout << buffer << '\n';
					buffer.clear();
					std::unique_lock<std::mutex> okLock(ok_cv_m);
					ok_cv.notify_one();
				}
			}
			else toParse.append(buffer);
			if (direct == true) {
				std::cout.write(buffer.c_str(), len);
				std::cout << '\n';
				toParse.clear();
			}
			buffer.clear();
			toParseMtx.unlock();
		}
		catch (boost::exception& e) {
			std::cout << "Exception thrown reading from socket. " << boost::diagnostic_information(e) << '\n';
			return;
		}
	}
	return;
}

void config() {
	bool ask = true;
	char saveConfirmation;
	std::cout << '\n';
	std::cout << "Enter your UID: ";
	std::cin >> uid;
	while (ask) {
		std::cout << "\nWould you like to save to file? (Y/N): ";
		std::cin >> saveConfirmation;
		switch (saveConfirmation) {
		case 'Y': { std::ofstream configuration("uid.txt"); configuration << uid; configuration.close(); std::cout << "ok\n\n"; ask = false; break; }
		case 'y': { std::ofstream configuration("uid.txt"); configuration << uid; configuration.close(); std::cout << "ok\n\n"; ask = false; break; }
		case 'N': { std::cout << "ok\n\n"; ask = false; break; }
		case 'n': { std::cout << "ok\n\n"; ask = false; break; }
		default: { std::cout << "Invalid choice. \n"; }
		}
	}
	std::cin.clear();
	std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
	return;
}

void help() {
	std::cout << '\n';
	std::cout << "lastmod10     - Display your last 10 modified visual novels. \n";
	std::cout << "playing       - Display your last 10 modified visual novels with label \"Playing\" \n";
	std::cout << "stalled       - Display your last 10 modified visual novels with label \"Stalled\" \n";
	std::cout << "wishlist      - Display your last 10 modified visual novels with label \"Wishlist\" \n";
	std::cout << "lookup-vn     - Enter visual novel lookup mode \n";
	std::cout << "lookup-user   - Enter user lookup mode \n";
	std::cout << "lookup-other  - Enter extended lookup mode \n";
	std::cout << "quote         - Get a random quote from VNDB \n";
	std::cout << "dbstats       - Display VNDB database statistics \n";
	std::cout << "help          - Display this help menu \n";
	std::cout << "config        - Configure your UID \n";
	std::cout << "direct        - Enter direct mode with VNDB server \n";
	std::cout << "exit          - Exit \n";
	std::cout << '\n';
	return;
}

void menu() {
	// wait until server sends ok
	std::unique_lock<std::mutex> okLock(ok_cv_m);
	ok_cv.wait(okLock, [&] { return ok; });

	while (!exitSignal)
	{
		std::cout << ">";
		std::string menuBuffer;
		getline(std::cin, menuBuffer);
		if (menuBuffer == "lastmod10" && !uid.empty()) lastmod10();
		if (menuBuffer == "playing" && !uid.empty()) displayByLabel("Playing", "1");
		if (menuBuffer == "stalled" && !uid.empty()) displayByLabel("Stalled", "3");
		if (menuBuffer == "wishlist" && !uid.empty()) displayByLabel("Wishlist", "5");
		if (menuBuffer == "lookup-vn") lookupVN();
		if (menuBuffer == "lookup-user") lookupUser();
		if (menuBuffer == "lookup-other") lookup();
		if (menuBuffer == "quote") quote();
		if (menuBuffer == "dbstats") dbstats();
		if (menuBuffer == "help") help();
		if (menuBuffer == "config") config();
		if (menuBuffer == "direct") {
			std::cout << "Dropping to direct mode... use \"menu\" to return. \n\n";
			write();
		}
		if (menuBuffer == "exit") {
			exitSignal = true;
			boost::asio::write(mainsocket, boost::asio::buffer("logout"), error);
			boost::asio::write(mainsocket, boost::asio::buffer(eofbyteX, sizeof(char)), error);
		}
	}
	return;
}

int main()
{
#ifdef _WIN32
	SetConsoleOutputCP(CP_UTF8); // for correct display of the UTF-8 data sent by vndb server in windows command prompt
#endif
	std::cout << "cpp-vndb-console-userpage, compiled " << __DATE__ << ' ' << __TIME__ << "\n\n";
	std::fstream configuration("uid.txt");
	if (configuration.good()) {
		std::cout << "Loading config... \n";
		getline(configuration, uid);
		std::cout << "Your UID is set to " << uid << ". Use config to edit. \n";
		configuration.close();
	}
	else {
		std::cout << "There is no UID set. No user commands will work other than direct. \n";
		std::cout << "Set your UID with config. \n";
	}
	std::thread readThread(read);
	std::thread menuThread(menu);
	readThread.join();
	menuThread.join();
	return 0;
}
