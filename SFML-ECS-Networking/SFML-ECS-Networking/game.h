#pragma once
#include "tank.h"

class Game
{
public:
	Game();

	void LoadTextures();
	void HandleEvents(const std::optional<sf::Event> event);
	void Update(float dt);
	// Network update now gets a collection of tank messages for multiple tanks.
	void NetworkUpdate(float dt, TankMessage data);
	void Render(sf::RenderWindow &window);
	void AddTank(std::string body_tex, std::string barrel_tex, sf::Vector2f position);
	TankMessage GetNetworkUpdate();

private:
	// We can now have more thank 1 tank in the game.
	std::vector<std::unique_ptr<Tank>> tanks;
	std::unique_ptr<sf::Sprite> background;
	// We'll hold all textures here so we only need to load them once at game creation.
	std::unordered_map<std::string, std::shared_ptr<sf::Texture>> textures;
};

