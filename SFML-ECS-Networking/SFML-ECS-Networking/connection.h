#pragma once

#include <SFML\Network.hpp>
#include <deque>
#include <memory>

enum PredictionMethod { None, Linear, Quadratic, Cubic };
enum ConnState { NotReady, Ready, Disconnected };

// Linear interpolation between two positions: returns v0 when t is 0 and v1 when t is 1.
// This is the "precise method" from the Wikipedia page, (1 - t) * v0 + t * v1. The form we used before,
// v0 + t * (v1 - v0), can be slightly off because of floating-point rounding, so it isn't guaranteed to
// give exactly v1 when t is 1. This one is.
inline sf::Vector2f Lerp(sf::Vector2f v0, sf::Vector2f v1, float t)
{
	return v0 * (1.f - t) + v1 * t;
}

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
		// Calculate interpolation step: the fraction of the time between the last two messages
		// that this frame covers.
		float step = 0.f;

		if (message_history.size() > 1) { // Make sure we have at least 2 messages for Linear interpolation.
			float gap = message_history[0].time - message_history[1].time;
			// If the timestamps are equal (or out of order) there is no usable gap to interpolate across,
			// so jump straight to the newest position instead of dividing by zero.
			step = (gap > 0.f) ? dt / gap : 1.f;
		}

		// Add step to inter_t
		inter_t += step;

		// Clamp inter_t to max of 1.f
		if (inter_t > 1.f)
			inter_t = 1.f;

		if (message_history.size() > 1) { // Make sure we have at least 2 messages for Linear interpolation.
			// Calculate new position by interpolating between last two known positons.
			return Lerp(message_history[1].position, message_history[0].position, inter_t);
		}
		else
			return message_history[0].position; // Just return the last known position.
	}
};