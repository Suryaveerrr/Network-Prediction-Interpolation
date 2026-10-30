#pragma once

#include <SFML\Network.hpp>
#include <algorithm>
#include <deque>
#include <memory>

// How we guess where a tank is right now, from the messages we have already received.
// (Cubic isn't implemented. It falls back to Quadratic.)
enum PredictionMethod { None, Linear, Quadratic, Cubic };

// How the Observer works out where to draw a tank.
enum SmoothingMode {
	InterpolateOnly,        // Glide between the last two known positions. Smooth, but a whole update behind.
	PredictOnly,            // Draw the predicted position. Up to date, but jumps when a prediction was wrong.
	PredictAndInterpolate   // Draw the predicted position, but glide from the old guess to the corrected one.
};

enum ConnState { NotReady, Ready, Disconnected };

// Linear interpolation between two positions: returns v0 when t is 0 and v1 when t is 1.
// This is the "precise method" from the Wikipedia page, (1 - t) * v0 + t * v1. The form we used before,
// v0 + t * (v1 - v0), can be slightly off because of floating-point rounding, so it isn't guaranteed to
// give exactly v1 when t is 1. This one is.
inline sf::Vector2f Lerp(sf::Vector2f v0, sf::Vector2f v1, float t)
{
	return v0 * (1.f - t) + v1 * t;
}

// Everything the Observer knows about one connected player, including which tank is theirs.
// This could also be made int oa class, given the increased functionality.
struct Connection
{
	// How many messages we keep. Quadratic prediction needs three positions, so we need at least 3.
	static constexpr std::size_t MAX_HISTORY = 3;
	// The longest we will predict ahead for. If messages stop coming (or the player has gone) we don't want
	// the tank flying off the screen.
	static constexpr float MAX_PREDICTION_TIME = 1.f;
	// How long, in seconds, the drawn position takes to glide to the corrected prediction after a new message.
	static constexpr float CORRECTION_TIME = 0.2f;

	std::unique_ptr<sf::TcpSocket> socket;
	ConnState state = Disconnected;

	// ID of this player's tank in the Game. It stays -1 until the player's first message arrives and we
	// know where the tank is. We use this instead of assuming the order of connections matches the order
	// of tanks, because connections get removed and the order changes.
	int tankId = -1;

	// Packets waiting to be sent to this player. Sockets are non-blocking, so a send may only take part
	// of a packet. A packet stays at the front until it has gone out completely.
	std::deque<sf::Packet> outbox;

	std::deque<TankMessage> message_history; // Newest message at the front.
	float inter_t = 0.f; // This value goes from 0.f to 1.f during interpolation and will be accumulated based on time delta.

	// Seconds since the newest message arrived, measured on the Observer's own clock. We can't work this
	// out from the timestamps in the messages, because those come from the Player's clock, which isn't
	// the same as ours.
	float time_since_message = 0.f;

	// Where the tank was last drawn, and the correction that glides from the old drawn position to the
	// newly predicted one after each message.
	sf::Vector2f displayed_position = { 0.f, 0.f };
	sf::Vector2f correction_start = { 0.f, 0.f };
	float correction_t = 1.f; // 0 = just started correcting, 1 = finished.

	// Store a newly received message and restart the timers that depend on it.
	void AddMessage(const TankMessage& message) {
		if (message_history.empty()) {
			// First message: there is nothing to glide from, so start right on it.
			displayed_position = message.position;
			correction_t = 1.f;
		}
		else {
			// Glide from wherever the tank is being drawn right now.
			correction_start = displayed_position;
			correction_t = 0.f;
		}

		inter_t = 0.f; // reset interpolation t value when we get a new message
		time_since_message = 0.f;

		message_history.push_front(message); // newest message added to the front
		if (message_history.size() > MAX_HISTORY)
			message_history.pop_back(); // remove the oldest message at the back
	}

	// Guess where the tank is right now, from the messages we have. Needs at least one message.
	sf::Vector2f Predict(PredictionMethod method) const {
		const TankMessage& newest = message_history[0];

		// No prediction, or not enough history to predict from: just the last known position.
		if (method == None || message_history.size() < 2)
			return newest.position;

		// How far ahead of the newest message we are predicting (capped, see MAX_PREDICTION_TIME).
		float t = std::min(time_since_message, MAX_PREDICTION_TIME);

		// Velocity from the last two positions. The times come from the Player's clock, but we only use
		// the difference between them, so the clocks don't need to match.
		float dt_new = newest.time - message_history[1].time;
		if (dt_new <= 0.f)
			return newest.position; // No usable time gap, so no usable velocity.
		sf::Vector2f velocity_new = (newest.position - message_history[1].position) / dt_new;

		// Linear: keep going at that velocity.
		if (method == Linear || message_history.size() < 3)
			return newest.position + velocity_new * t;

		// Quadratic (and Cubic, which isn't implemented): also use the velocity before that, to get
		// acceleration, and predict with position + velocity * t + 0.5 * acceleration * t * t.
		float dt_old = message_history[1].time - message_history[2].time;
		if (dt_old <= 0.f)
			return newest.position + velocity_new * t; // Can't get acceleration, so fall back to linear.
		sf::Vector2f velocity_old = (message_history[1].position - message_history[2].position) / dt_old;

		// Each velocity is the average over its own gap, so they are half of each gap apart from
		// the middle of one to the middle of the next. That's the time over which velocity changed.
		sf::Vector2f acceleration = (velocity_new - velocity_old) / ((dt_old + dt_new) * 0.5f);

		return newest.position + velocity_new * t + acceleration * (0.5f * t * t);
	}

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

	// Work out where to draw the tank this frame. Call once per frame, and only when we have a message.
	sf::Vector2f GetDisplayPosition(float dt, SmoothingMode mode, PredictionMethod method) {
		time_since_message += dt;

		sf::Vector2f position;

		if (mode == InterpolateOnly) {
			position = Interpolate(dt);
		}
		else if (mode == PredictOnly) {
			position = Predict(method);
		}
		else {
			// PredictAndInterpolate: the target is the prediction for right now, which keeps moving as
			// time passes. After a new message the target may be somewhere else than where we were
			// drawing the tank, so glide from the old drawn position to the target instead of snapping.
			sf::Vector2f target = Predict(method);
			correction_t = std::min(correction_t + dt / CORRECTION_TIME, 1.f);
			position = Lerp(correction_start, target, correction_t);
		}

		displayed_position = position;
		return position;
	}
};