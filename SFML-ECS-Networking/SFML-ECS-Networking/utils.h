#pragma once
#include <string>
#include <iostream>
#include <chrono>
#include <iomanip>
#include <ctime>

enum MessageType { info, debug, warning, error, success };
enum ConnType { Undefined, TCP, UDP };

class Utils
{
public:
	static void printMsg(std::string msg, MessageType type = info) {
		const auto now = std::chrono::system_clock::now();
		const std::time_t currentTime = std::chrono::system_clock::to_time_t(now);
		std::tm localTime = {};
		localtime_s(&localTime, &currentTime);
		const auto timestamp = std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");

		switch (type)
		{
		case debug: std::cout << "\033[36m" << timestamp << "   " << msg << "\033[0m" << std::endl; break;
		case warning: std::cout << "\033[33m" << timestamp << "   " << msg << "\033[0m" << std::endl; break;
		case error: std::cout << "\033[1;31m" << timestamp << "   " << msg << "\033[0m" << std::endl; break;
		case success: std::cout << "\033[1;32m" << timestamp << "   " << msg << "\033[0m" << std::endl; break;
		default: std::cout << timestamp << "   " << msg << std::endl; break;
		}
	}
};