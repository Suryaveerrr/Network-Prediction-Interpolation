#pragma once
#include <cstdint>
#include <SFML/System/Vector2.hpp>

// Every packet starts with one of these, so the receiver knows what follows.
enum class PacketType : std::int32_t {
	Update,       // Player -> Observer: the Player's tank (time, x, y).
	TimeSync,     // Observer -> Player: the time on the Observer's clock.
	SyncRequest,  // Player -> Observer: "what time is it?"
	Ping,         // Player -> Observer: used to measure the round trip time.
	Pong          // Observer -> Player: the Observer's immediate reply to a Ping.
};

// A simple tank update message
// FIXME: Consider what else we need to send and include it here.
struct TankMessage {
	// The coordinates of the tank within the game world.
	float time = 0;
	int id = 0;
	// Changed position to a vector2 for ease of interpolation calculations
	sf::Vector2f position = { 0.f, 0.f };
};