#include "tank.h"

Tank::Tank(std::shared_ptr<sf::Texture> bodyTexture, std::shared_ptr<sf::Texture> barrelTexture)
{
	// Initialise sprites.
	body = std::make_unique<sf::Sprite>(*bodyTexture);
	barrel = std::make_unique<sf::Sprite>(*barrelTexture);

	// Set sprite origins. For body use the center of the texture. For barrel, hardcoded value.
	body->setOrigin((sf::Vector2f)body->getTextureRect().getCenter());
	barrel->setOrigin({ 6, 2 });

	// With the correct offset on the barrel, we can just set barrel position = body position.
	body->setPosition(position);
	barrel->setPosition(body->getPosition());

	// Set default barrel rotation to match body rotation.
	// FIXME: for actual tank game, we would have barrel rotated independently of the body.
	body->setRotation(bodyRotation);
	barrel->setRotation(body->getRotation());
}

void Tank::Update(float dt)
{
	// Update rotation angle based on input.
	if (isMoving.left)
		bodyRotation -= sf::degrees(rotationSpeed * dt);
	else if (isMoving.right)
		bodyRotation += sf::degrees(rotationSpeed * dt);

	// Calculate direction vector from angle of rotation.
	sf::Vector2f body_direction = {
		std::cos((bodyRotation - sf::degrees(90)).asRadians()),
		std::sin((bodyRotation - sf::degrees(90)).asRadians())
	};

	// Update position based on input and direction.
	if (isMoving.forward)
		position -= body_direction * movementSpeed * dt;
	else if (isMoving.backward)
		position += body_direction * movementSpeed * dt;

	// Apply new rotation to tank body and barrel.
	body->setRotation(bodyRotation);
	barrel->setRotation(bodyRotation);

	// Apply new position to tank body and barrel.
	body->setPosition(position);
	barrel->setPosition(position);
}

const void Tank::Render(sf::RenderWindow &window) {
		window.draw(*body);
		window.draw(*barrel);
}
