#include <SFML\Graphics.hpp>
#include <SFML\Network.hpp>
#include <sstream>
#include <iomanip>
#include "game.h"
#include "utils.h"
#include "connection.h"

/* Uncomment to enable debug messages. */
//#define DEBUG

/*
 This is a modified version of Lab 5 game, which now includes message history for each connection and
 interpolation of player positions on the observer-side.
 The player connects to the Observer, and sends updates about its tank position.
 The Observer accepts multiple connections from players, receives their tank position updates,
 and interpolates the tank positions based on message history to provide smooth movement.
 It can also predict where each tank is right now, from the last few positions.

 Keys on the Observer:  1 = interpolation only, 2 = prediction only, 3 = prediction and interpolation,
						4 = no prediction, 5 = linear prediction, 6 = quadratic prediction.
 Keys on the Player:    R = cycle through send rates (so you don't have to rebuild to test slow updates).
*/

static const char* SmoothingName(SmoothingMode mode) {
	switch (mode) {
	case InterpolateOnly: return "Interpolation only";
	case PredictOnly: return "Prediction only";
	default: return "Prediction + interpolation";
	}
}

static const char* PredictionName(PredictionMethod method) {
	switch (method) {
	case None: return "No prediction";
	case Linear: return "Linear prediction";
	default: return "Quadratic prediction";
	}
}

int main() {
	bool is_observer = false;
	unsigned short observer_port = 53000;
	unsigned short player_port = sf::Socket::AnyPort;

	Utils::printMsg("Game startup...");

	std::string title_type = "";

	bool ready = false;

	while (!ready) {
		Utils::printMsg("Are we playing or observing? Enter 1 for Player, enter 2 for Observer:");
		std::string input_line;
		std::getline(std::cin, input_line);

		if (!input_line.empty() && input_line.at(0) == '1') {
			Utils::printMsg("Player chosen, preparing window...");
			title_type = "Player";
			is_observer = false;
			ready = true;
		}
		else if (!input_line.empty() && input_line.at(0) == '2') {
			Utils::printMsg("Observer chosen, preparing window...");
			title_type = "Observer";
			is_observer = true;
			ready = true;
		}
		else {
			Utils::printMsg("Incorrect input, please try again!", warning);
		}
	}

	// Prepare window.
	sf::RenderWindow window(sf::VideoMode({ 640, 480 }), "CMP425/CMP501 - Lab 6: Prediction & Interpolation (" + title_type + ")");
	window.setFramerateLimit(60);	//Request 60 frames per second
	Utils::printMsg("Window ready, configuring networking...");

	// Prepare font and text object for debugging text.
	sf::Font font;
	if (!font.openFromFile("Assets/Montserrat-Regular.ttf"))
		Utils::printMsg("Could not load the font, the on-screen text will not show.", warning);
	sf::Text text(font);
	text.setCharacterSize(18);
	text.setFillColor(sf::Color::Red);
	text.setStyle(sf::Text::Bold | sf::Text::Underlined);

	// Prepare networking.
	sf::TcpListener listener;
	// Socket for communication with observer
	std::unique_ptr<sf::TcpSocket> player_socket = std::make_unique<sf::TcpSocket>();

	auto port = is_observer ? observer_port : player_port;
	listener.setBlocking(false); // set listener to non-blocking mode so we can keep checking for connections

	if (listener.listen(port) == sf::Socket::Status::Error) {
		// FIXME: we need to handle the error and make sure we don't try to accept, etc. if the listen fails.
	}

	// Clock for timing the 'dt' value
	sf::Clock clock;
	float game_time = 0;


	// Game object.
	Game game;
	// Container of player connections for Observer.
	std::vector<Connection> player_connections;

	// Other game parameters.
	// The Player can cycle through these with R, to test how slow updates look.
	const float send_rates[] = { 0.1f, 0.25f, 0.5f, 1.f };
	int send_rate_index = 0;
	float send_rate = send_rates[send_rate_index];
	float reconnect_delay = 1.f;
	float send_timer = 1.f;
	bool is_connected = false;

	// How the Observer works out where to draw tanks. Change them with the number keys while running.
	SmoothingMode smoothing_mode = PredictAndInterpolate;
	PredictionMethod prediction_method = Linear;

	// Selector for handling multiple connections (Observer only).
	sf::SocketSelector selector;

	while (window.isOpen()) {
		// Calculate dt.
		float dt = clock.restart().asSeconds();
		// Count how much time has passed.
		game_time += dt;

		std::string overlay = "Game Time: " + std::to_string(game_time);
		if (is_observer) {
			overlay += "\n" + std::string(SmoothingName(smoothing_mode)) + " / " + PredictionName(prediction_method);
			overlay += "\nKeys 1-3: mode, 4-6: prediction";
		}
		else {
			overlay += "\nSending every " + std::to_string(send_rate) + "s (R to change)";
		}
		text.setString(overlay);

		send_timer += dt;
		// Handle window events (e.g. key press).
		while (const std::optional event = window.pollEvent()) {
			if (event->is<sf::Event::Closed>()) {
				Utils::printMsg("Window closed, unbinding socket...", MessageType::warning);
				window.close();
				listener.close();
			}
			if (const auto* keyPressed = event->getIf<sf::Event::KeyPressed>()) {
				if (keyPressed->scancode == sf::Keyboard::Scancode::Escape) {
					Utils::printMsg("ESC pressed, closing window, unbinding socket...", MessageType::warning);
					window.close();
					listener.close();
				}

				// Observer: choose how tanks are drawn.
				if (is_observer) {
					bool changed = true;
					switch (keyPressed->scancode) {
					case sf::Keyboard::Scancode::Num1: smoothing_mode = InterpolateOnly; break;
					case sf::Keyboard::Scancode::Num2: smoothing_mode = PredictOnly; break;
					case sf::Keyboard::Scancode::Num3: smoothing_mode = PredictAndInterpolate; break;
					case sf::Keyboard::Scancode::Num4: prediction_method = None; break;
					case sf::Keyboard::Scancode::Num5: prediction_method = Linear; break;
					case sf::Keyboard::Scancode::Num6: prediction_method = Quadratic; break;
					default: changed = false; break;
					}
					if (changed)
						Utils::printMsg(std::string("Now using: ") + SmoothingName(smoothing_mode) + " / " + PredictionName(prediction_method), MessageType::info);
				}
				// Player: cycle the send rate.
				else if (keyPressed->scancode == sf::Keyboard::Scancode::R) {
					send_rate_index = (send_rate_index + 1) % 4;
					send_rate = send_rates[send_rate_index];
					Utils::printMsg("Send rate is now every " + std::to_string(send_rate) + " seconds.", MessageType::info);
				}
			}
			// We only handle inputs for the player. Observer will do direct updates from network data.
			if (!is_observer)
				game.HandleEvents(event);
		}
		// Packet to hold our updates.
		sf::Packet packet;

		if (is_observer) { // OBSERVER
			// Socket for communication with player
			std::unique_ptr<sf::TcpSocket> socket = std::make_unique<sf::TcpSocket>();

			// Check for incoming connections
			if (listener.accept(*socket) == sf::Socket::Status::Done) {
				Utils::printMsg("New connection from: "
					+ socket->getRemoteAddress().value().toString()
					+ ":"
					+ std::to_string(socket->getRemotePort()));

				// Add connection the vector.
				player_connections.push_back({ std::move(socket) });
				selector.add(*player_connections.back().socket); // Add to persistent selector
				player_connections.back().socket->setBlocking(false);
				// Set the deafult connection state to NotReady.
				player_connections.back().state = NotReady;

				// Add new tank with blue body and black barrel at the centre of the window.
				// FIXME: Ideally we should wait until the first receive gets some data about the tank
				// position and colour before we add it to the Observer's game.
				game.AddTank("body_blue", "barrel_black", { 320, 240 });

				//This is a bit of a hack, we want to reset the game time when the first player connects to "sync up the game clocks".
				// FIXME: This will only work for the first player. As more player connect, they will be out of sync with the game time. 
				//		  We should handle this better, e.g. by sending the current game time to the new player and having them start from 
				//		  that time.
				if (player_connections.size() == 1)
					game_time = 0.f;

			}

			// Reset states before waiting to avoid stale Ready state causing a blocking receive.
			for (size_t i = 0; i < player_connections.size(); ++i)
				player_connections[i].state = NotReady;

			// Check which connections have some data waiting to be received from sockets.
			if (selector.wait(sf::milliseconds(5))) {
				// If at least one of the sockets is ready to receive, check which ones are ready.
				for (int i = 0; i < player_connections.size(); i++) {
					if (selector.isReady(*player_connections[i].socket))
						player_connections[i].state = Ready;
				}
			}

			// FIXME: we never remove disconnected players from the vector of connections. This will
			// cause us to continue to update and draw them. Handle disconnections and fix this.
			for (int i = 0; i < player_connections.size(); i++) {
				// FIXME: Recieve can fail, check for errors.
				if (player_connections[i].state == Ready) {
					sf::Socket::Status status = player_connections[i].socket->receive(packet);
					if (status == sf::Socket::Status::Done) {
#ifdef DEBUG
						Utils::printMsg("Recieved message from: "
							+ player_connections[i].socket->getRemoteAddress().value().toString()
							+ ":"
							+ std::to_string(player_connections[i].socket->getRemotePort()));
#endif // DEBUG
						TankMessage message;
						message.id = i;
						// Read recieved data into TankMessage struct.
						// FIXME: reading from packet can fail. Refer to documentation on how to
						// handle errors and add error checking here.
						packet >> message.time >> message.position.x >> message.position.y;

						// Store the message in the message_history (this also restarts the interpolation
						// and prediction timers, and starts the correction glide).
						player_connections[i].AddMessage(message);
#ifdef DEBUG
						//For DEBUG ONLY, pring current message history. Remove this when no longer needed!
						std::string history_msg = "Current queue: \n";
						for (int m = 0; m < player_connections[i].message_history.size(); m++) {
							history_msg.append("\t\t{ Time: ");
							history_msg.append(std::to_string(player_connections[i].message_history[m].time));
							history_msg.append(", X: ");
							history_msg.append(std::to_string(player_connections[i].message_history[m].position.x));
							history_msg.append(", Y: ");
							history_msg.append(std::to_string(player_connections[i].message_history[m].position.y));
							history_msg.append(" }");
							if (m != (player_connections[i].message_history.size() - 1))
								history_msg.append("\n");
						}

						Utils::printMsg(history_msg, MessageType::debug);
#endif // DEBUG
					}
					else if (status == sf::Socket::Status::Disconnected) {
						Utils::printMsg("Player disconnected: "
							+ player_connections[i].socket->getRemoteAddress().value().toString()
							+ ":"
							+ std::to_string(player_connections[i].socket->getRemotePort()), warning);

						// For now just set the connection state to Disconnected. 
						// FIXME: But we should also remove the inactive connection from the vector and remove the tank from the game.
						player_connections[i].state = Disconnected;
					}
				}
				// NOTE: Moved player update here, as we want to update every time, not just when we recieve messages.
				if (!player_connections[i].message_history.empty()) {
#ifdef DEBUG
					Utils::printMsg("dt = " + std::to_string(dt) + ", Inter t = " + std::to_string(player_connections[i].inter_t), MessageType::info);
#endif // DEBUG
					// Get the position to draw, using the interpolation / prediction method chosen with the keys.
					sf::Vector2f display_pos = player_connections[i].GetDisplayPosition(dt, smoothing_mode, prediction_method);

					// Construct message with actual positon.
					TankMessage display_msg = {
						game_time,
						player_connections[i].message_history[0].id,
						display_pos
					};
					// Update game using message containign actual position update.
					game.NetworkUpdate(dt, display_msg);
				}
			}
		}
		else // PLAYER
		{
			// If we're not connected to the observer, try to connect.
			// We want the socket in non-blocking mode, so if the connection fails, we just try again next time.
			if (!is_connected) {
				Utils::printMsg("Attempting connection to observer...");
				// SFML TCP sockets are fiddly when it comes to non-blocking connect calls,
				// we could specify a very small delay timeout in the blocking mode, but this would cause lag while it tries.
				// Ideally, we just handle connection in blocking mode, and wait for a successful connection before we continue.
				sf::Socket::Status status = player_socket->connect(sf::IpAddress::LocalHost, observer_port);
				if (status == sf::Socket::Status::Done) {
					Utils::printMsg("Connected!", MessageType::success);
					is_connected = true;
					// Add new tank with blue body and black barrel at the centre of the window.
					game.AddTank("body_blue", "barrel_black", { 320, 240 });
				}
				else {
					Utils::printMsg("Failed to connect to observer. Will try again later.", MessageType::warning);
				}
				send_timer = 0;
			}
			else {
				game.Update(dt);
				// Get data structure from game, containing update message.
				TankMessage message = game.GetNetworkUpdate();
				// Add current game time to the message
				message.time = game_time;
				// Translate our messgage struct to sf::Packet (very rudimentary conversion).
				packet << message.time << message.position.x << message.position.y;

				// Send messages only as often as the send rate allows.
				if (send_timer >= send_rate) {
					sf::Socket::Status status = player_socket->send(packet);
					// FIXME: Send can fail, check for errors and adjust logic accordingly.
					if (status == sf::Socket::Status::Done) {
#ifdef DEBUG
						Utils::printMsg("Sent message to: "
							+ player_socket->getRemoteAddress().value().toString()
							+ ":"
							+ std::to_string(observer_port));
#endif // DEBUG
					}
					else {
						Utils::printMsg("Failed to send message to Observer", error);
					}
					// Reset timer after sending the message
					send_timer = 0;
				}
			}
		}
		// Render
		window.clear();
		game.Render(window); // This takes window as a reference.
		window.draw(text);
		window.display();
	}

	return 0;
}