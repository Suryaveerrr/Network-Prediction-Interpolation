#pragma once

// A simple tank update message
// FIXME: Consider what else we need to send and include it here.
struct TankMessage {
	// The coordinates of the tank within the game world.
	float time = 0;
	int id = 0;
	// Changed position to a vector2 for ease of interpolation calculations
	sf::Vector2f position = {0.f, 0.f};
};