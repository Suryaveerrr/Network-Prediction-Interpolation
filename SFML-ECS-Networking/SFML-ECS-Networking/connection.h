#pragma once

#include <SFML\Network.hpp>
#include <deque>

enum PredictionMethod { None, Linear, Quadratic, Cubic };
enum ConnState {NotReady, Ready, Disconnected};

// Fixme:: ideally we should link specific tank/character to each connection,
// rather than assuming the order in which connections are made is the same as
// the order in which tanks/characters are stored in the game.

// This could also be made int oa class, given the increased functionality.
struct Connection
{
	std::unique_ptr<sf::TcpSocket> socket;
	ConnState state = Disconnected;

	std::deque<TankMessage> message_history;
	float inter_t = 0.f; // This value goes from 0.f to 1.f during interpolation and will be accumulated based on time delta.

	// Interpolate position based on selected method and value of t. Return TankMessage with updated position.
	sf::Vector2f Interpolate(float dt) {
		// Calculate interpolation step
		float step = 0.f;
		
		if(message_history.size() > 1) // Make sure we have at least 2 messages for Linear interpolation.
			step = 1.f / ((message_history[0].time - message_history[1].time) / dt);
		else
			step = 0.f;

		// Add step to inter_t
		inter_t += step;

		// Clamp inter_t to max of 1.f
		if(inter_t > 1.f)
			inter_t = 1.f;

		if (message_history.size() > 1) { // Make sure we have at least 2 messages for Linear interpolation.
			// Calculate new position by interpolating between last two known positons.
			return (message_history[1].position + (message_history[0].position - message_history[1].position) * inter_t);
		}
		else
			return message_history[0].position; // Just return the last known position.
	}
};