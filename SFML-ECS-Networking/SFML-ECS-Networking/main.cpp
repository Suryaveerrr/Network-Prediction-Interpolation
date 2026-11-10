#include <SFML\Graphics.hpp>
#include <SFML\Network.hpp>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <vector>
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
 The Player also measures the round trip time to the Observer, with pings.

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

// Turns a time in seconds into text in milliseconds, e.g. 0.0334 -> "33.4 ms".
static std::string FormatMs(float seconds) {
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(1) << seconds * 1000.f << " ms";
	return stream.str();
}

// Sends as much of the queue as the socket will take right now, front to back. A packet is only removed
// once it has been sent completely. If the socket takes only part of it (Partial) or none of it
// (NotReady), we stop. The same, unchanged packet is then sent again next time, and SFML carries on from
// where it left off. Returns Done if the queue is empty, otherwise the status that stopped us.
static sf::Socket::Status FlushOutbox(sf::TcpSocket& socket, std::deque<sf::Packet>& outbox)
{
	while (!outbox.empty()) {
		sf::Socket::Status status = socket.send(outbox.front());
		if (status != sf::Socket::Status::Done)
			return status;
		outbox.pop_front();
	}
	return sf::Socket::Status::Done;
}

int main() {
	bool is_observer = false;
	unsigned short observer_port = 53000;

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
	sf::RenderWindow window(sf::VideoMode({ 640, 480 }), "CMP425/CMP501 - Lab 7: Synchronisation (" + title_type + ")");
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

	// Only the Observer listens for connections. Players connect to it, so they don't need a listener.
	if (is_observer) {
		listener.setBlocking(false); // set listener to non-blocking mode so we can keep checking for connections

		// Without a listener the Observer can never accept a Player, so there is no point carrying on.
		if (listener.listen(observer_port) != sf::Socket::Status::Done) {
			Utils::printMsg("Could not listen on port " + std::to_string(observer_port)
				+ ". Is another Observer already running?", error);
			return 1;
		}
		Utils::printMsg("Listening on port " + std::to_string(observer_port));
	}

	// Clock for timing the 'dt' value
	sf::Clock clock;
	float game_time = 0;


	// Game object.
	Game game;

	// A Player has its own tank from the start, so it can drive even before it has connected.
	if (!is_observer)
		game.SetLocalTank(game.AddTank("body_blue", "barrel_black", { 320, 240 }));

	// Container of player connections for Observer.
	std::vector<Connection> player_connections;

	// Other game parameters.
	// The Player can cycle through these with R, to test how slow updates look.
	const float send_rates[] = { 0.1f, 0.25f, 0.5f, 1.f };
	int send_rate_index = 0;
	float send_rate = send_rates[send_rate_index];
	float reconnect_delay = 1.f;
	float reconnect_timer = reconnect_delay; // Start "due", so the first attempt happens straight away.
	float send_timer = 1.f;
	bool is_connected = false;

	// Player: whether we have adopted the Observer's clock yet, and the packets waiting to go to the
	// Observer (with a limit on how many we let build up if the connection is struggling).
	bool clock_synced = false;
	std::deque<sf::Packet> outbox;
	const std::size_t max_queued_updates = 4;

	// Player: measuring the round trip time (RTT) to the Observer. We send a few pings, one at a time.
	// The Observer sends each one straight back, and the time that takes is one RTT sample.
	const std::size_t ping_count = 5;   // How many samples we average over.
	const float ping_timeout = 2.f;     // Give up waiting for a pong after this many seconds.
	// This clock times one ping at a time. It is separate from game_time, because game_time gets adjusted
	// when we sync with the Observer, and a clock that jumps would ruin the measurement.
	sf::Clock ping_clock;
	std::int32_t next_ping_id = 0;
	bool ping_in_flight = false;
	std::vector<float> rtt_samples;
	bool latency_measured = false;
	float rtt_average = 0.f, rtt_min = 0.f, rtt_max = 0.f;

	// How the Observer works out where to draw tanks. Change them with the number keys while running.
	SmoothingMode smoothing_mode = PredictAndInterpolate;
	PredictionMethod prediction_method = Linear;

	// Selector for handling multiple connections (Observer only).
	sf::SocketSelector selector;

	// Observer: stops dealing with a player (they left, or their connection broke) and removes their tank.
	// The connection itself is erased from the vector at the end of the frame.
	auto drop_connection = [&](Connection& connection, sf::Socket::Status why) {
		// Work out who this was before we close the socket (the address is gone afterwards).
		auto remote_address = connection.socket->getRemoteAddress();
		std::string who = remote_address
			? remote_address->toString() + ":" + std::to_string(connection.socket->getRemotePort())
			: "unknown address";

		if (why == sf::Socket::Status::Disconnected)
			Utils::printMsg("Player disconnected: " + who, warning);
		else
			Utils::printMsg("Connection error, dropping player: " + who, error);

		// Stop watching the socket (before closing it, the selector needs the handle), then close it.
		// A closed connection always counts as "ready", so leaving it in the selector would flag it
		// every frame.
		selector.remove(*connection.socket);
		connection.socket->disconnect();
		connection.state = Disconnected;

		// Remove their tank, so it is no longer updated or drawn.
		if (connection.tankId >= 0)
			game.RemoveTank(connection.tankId);
		};

	// Player: forget any latency measurement, so the next connection starts measuring afresh.
	auto reset_latency_measurement = [&]() {
		latency_measured = false;
		ping_in_flight = false;
		next_ping_id = 0;
		rtt_samples.clear();
		};

	// Player: the connection to the Observer has broken. Go back to trying to connect.
	auto lose_connection = [&](const std::string& reason) {
		Utils::printMsg(reason + " Will try to reconnect.", error);
		player_socket->disconnect();
		is_connected = false;
		clock_synced = false;
		outbox.clear();
		reset_latency_measurement();
		reconnect_timer = 0;
		};

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
			if (!is_connected)
				overlay += "\nNot connected";
			else if (latency_measured)
				overlay += "\nRTT avg " + FormatMs(rtt_average) + " (min " + FormatMs(rtt_min) + ", max " + FormatMs(rtt_max) + ")";
			else
				overlay += "\nMeasuring latency... " + std::to_string(rtt_samples.size()) + "/" + std::to_string(ping_count);
		}
		text.setString(overlay);

		send_timer += dt;
		reconnect_timer += dt;
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

				// There is no tank yet: it is added when the player's first message arrives, so it starts
				// where the player's tank really is. (The colour isn't part of these messages, so the tank
				// is always blue. Lab 5 sends it in a separate message.)

				// Tell the new player what time it is on our clock, so every player's timestamps share the
				// Observer's time. It ignores how long the message takes to arrive, which is tiny on
				// localhost. (The next steps replace this with a proper measurement.)
				sf::Packet welcome;
				welcome << static_cast<std::int32_t>(PacketType::TimeSync) << game_time;
				player_connections.back().outbox.push_back(welcome);
			}

			// Reset states before waiting to avoid stale Ready state causing a blocking receive.
			// Disconnected is final, so those connections are left alone.
			for (size_t i = 0; i < player_connections.size(); ++i) {
				if (player_connections[i].state != Disconnected)
					player_connections[i].state = NotReady;
			}

			// Check which connections have some data waiting to be received from sockets.
			if (selector.wait(sf::milliseconds(5))) {
				// If at least one of the sockets is ready to receive, check which ones are ready.
				for (size_t i = 0; i < player_connections.size(); i++) {
					// Check the state first: a disconnected socket has been removed from the selector
					// and closed, so we must not ask the selector about it.
					if (player_connections[i].state != Disconnected && selector.isReady(*player_connections[i].socket))
						player_connections[i].state = Ready;
				}
			}

			for (size_t i = 0; i < player_connections.size(); i++) {
				Connection& connection = player_connections[i];

				if (connection.state == Ready) {
					// Read everything that has arrived on this socket.
					while (true) {
						sf::Packet packet;
						sf::Socket::Status status = connection.socket->receive(packet);

						if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error) {
							// The player left or the connection broke: remove them and their tank.
							drop_connection(connection, status);
							break;
						}
						if (status != sf::Socket::Status::Done) {
							// NotReady means nothing more to read. Partial means only part of a packet has
							// arrived, and SFML keeps what it has until the rest shows up on a later receive.
							break;
						}
#ifdef DEBUG
						Utils::printMsg("Recieved message from: "
							+ connection.socket->getRemoteAddress().value().toString()
							+ ":"
							+ std::to_string(connection.socket->getRemotePort()));
#endif // DEBUG
						// Every packet starts with its type, so we know how to read the rest.
						std::int32_t tag;
						if (!(packet >> tag)) {
							Utils::printMsg("Received a packet with no type tag, ignoring it.", warning);
							continue;
						}
						PacketType type = static_cast<PacketType>(tag);

						// A ping: send it straight back, with the same number, so the player can time the round trip.
						// This is queued now and goes out at the end of this frame, so the reply is as quick
						// as we can make it.
						if (type == PacketType::Ping) {
							std::int32_t ping_id;
							if (!(packet >> ping_id)) {
								Utils::printMsg("Malformed Ping packet, ignoring it.", warning);
								continue;
							}
							sf::Packet pong;
							pong << static_cast<std::int32_t>(PacketType::Pong) << ping_id;
							connection.outbox.push_back(pong);
							continue;
						}

						if (type != PacketType::Update) {
							Utils::printMsg("Unexpected packet type from a player, ignoring it.", warning);
							continue;
						}

						TankMessage message;
						// Read recieved data into TankMessage struct. Reading can fail if the packet is
						// too short, so check it before using the values.
						if (!(packet >> message.time >> message.position.x >> message.position.y)) {
							Utils::printMsg("Malformed packet from a player, ignoring it.", warning);
							continue;
						}

						// The first message gives us the tank's real position, so add the tank now.
						if (connection.tankId < 0) {
							connection.tankId = game.AddTank("body_blue", "barrel_black", message.position);
							Utils::printMsg("Player " + std::to_string(connection.tankId) + " joined.", success);
						}
						// The connection knows which tank is its own, so we don't rely on its position in the vector.
						message.id = connection.tankId;

						// Store the message in the message_history (this also restarts the interpolation
						// and prediction timers, and starts the correction glide).
						connection.AddMessage(message);
#ifdef DEBUG
						//For DEBUG ONLY, pring current message history. Remove this when no longer needed!
						std::string history_msg = "Current queue: \n";
						for (size_t m = 0; m < connection.message_history.size(); m++) {
							history_msg.append("\t\t{ Time: ");
							history_msg.append(std::to_string(connection.message_history[m].time));
							history_msg.append(", X: ");
							history_msg.append(std::to_string(connection.message_history[m].position.x));
							history_msg.append(", Y: ");
							history_msg.append(std::to_string(connection.message_history[m].position.y));
							history_msg.append(" }");
							if (m != (connection.message_history.size() - 1))
								history_msg.append("\n");
						}

						Utils::printMsg(history_msg, MessageType::debug);
#endif // DEBUG
					}
				}

				// NOTE: Moved player update here, as we want to update every time, not just when we recieve messages.
				if (connection.state != Disconnected && !connection.message_history.empty()) {
#ifdef DEBUG
					Utils::printMsg("dt = " + std::to_string(dt) + ", Inter t = " + std::to_string(connection.inter_t), MessageType::info);
#endif // DEBUG
					// Get the position to draw, using the interpolation / prediction method chosen with the keys.
					sf::Vector2f display_pos = connection.GetDisplayPosition(dt, smoothing_mode, prediction_method);

					// Construct message with actual positon.
					TankMessage display_msg = {
						game_time,
						connection.tankId,
						display_pos
					};
					// Update game using message containign actual position update.
					game.NetworkUpdate(dt, display_msg);
				}
			}

			// Send out whatever is queued for each player.
			for (auto& connection : player_connections) {
				if (connection.state == Disconnected || connection.outbox.empty())
					continue;

				sf::Socket::Status status = FlushOutbox(*connection.socket, connection.outbox);
				if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
					drop_connection(connection, status);
				// Partial and NotReady: the packet at the front stays queued and is sent again next frame.
			}

			// Disconnected players' sockets are closed and out of the selector, and their tanks are
			// gone from the game, so now it is safe to drop their connections from the vector too.
			player_connections.erase(
				std::remove_if(player_connections.begin(), player_connections.end(),
					[](const Connection& connection) { return connection.state == Disconnected; }),
				player_connections.end());
		}
		else // PLAYER
		{
			// The Player's own tank is always updated, connected or not.
			game.Update(dt);

			// If we're not connected to the observer, try to connect.
			if (!is_connected) {
				// Only try once per reconnect_delay, instead of on every single frame.
				if (reconnect_timer >= reconnect_delay) {
					reconnect_timer = 0;

					Utils::printMsg("Attempting connection to observer...");
					// SFML TCP sockets are fiddly when it comes to non-blocking connect calls,
					// we could specify a very small delay timeout in the blocking mode, but this would cause lag while it tries.
					// Ideally, we just handle connection in blocking mode, and wait for a successful connection before we continue.
					// So switch to blocking mode for each attempt (we go non-blocking again once connected).
					player_socket->setBlocking(true);
					sf::Socket::Status status = player_socket->connect(sf::IpAddress::LocalHost, observer_port);
					if (status == sf::Socket::Status::Done) {
						Utils::printMsg("Connected!", MessageType::success);
						is_connected = true;
						// From now on neither sending nor receiving may hold up the game loop.
						player_socket->setBlocking(false);
						// We start sending once we have the Observer's clock.
						clock_synced = false;
						outbox.clear();
						// Start measuring the latency to this Observer from scratch.
						reset_latency_measurement();
					}
					else {
						Utils::printMsg("Failed to connect to observer. Will try again later.", MessageType::warning);
					}
				}
			}
			else {
				// Listen for messages from the Observer: the time on its clock, and replies to our pings.
				while (is_connected) {
					sf::Packet incoming;
					sf::Socket::Status status = player_socket->receive(incoming);

					if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error) {
						lose_connection("Lost connection to Observer.");
						break;
					}
					if (status != sf::Socket::Status::Done)
						break; // NotReady, or only part of a packet so far: nothing more to read right now.

					// Every packet starts with its type, so we know how to read the rest.
					std::int32_t tag;
					if (!(incoming >> tag)) {
						Utils::printMsg("Received a packet with no type tag, ignoring it.", warning);
						continue;
					}
					PacketType type = static_cast<PacketType>(tag);

					if (type == PacketType::TimeSync) {
						float observer_time;
						if (!(incoming >> observer_time)) {
							Utils::printMsg("Malformed message from the Observer, ignoring it.", warning);
							continue;
						}

						// Adopt the Observer's clock, so all of our timestamps are on the same time scale as
						// every other player's.
						game_time = observer_time;
						clock_synced = true;
						Utils::printMsg("Clock synced to the Observer.", MessageType::success);
					}
					else if (type == PacketType::Pong) {
						std::int32_t pong_id;
						if (!(incoming >> pong_id)) {
							Utils::printMsg("Malformed Pong packet, ignoring it.", warning);
							continue;
						}
						// Only the reply to the ping we are waiting for counts. Anything else is a late reply
						// to a ping we gave up on.
						if (!ping_in_flight || pong_id != next_ping_id) {
							Utils::printMsg("Ignoring an old Pong.", warning);
							continue;
						}

						// The time since we sent the ping is one round trip.
						float rtt = ping_clock.getElapsedTime().asSeconds();
						rtt_samples.push_back(rtt);
						ping_in_flight = false;
						next_ping_id++;
						Utils::printMsg("Ping " + std::to_string(rtt_samples.size()) + ": " + FormatMs(rtt), MessageType::debug);

						// Once we have enough samples, work out the average (and the smallest and biggest).
						if (rtt_samples.size() >= ping_count) {
							float sum = 0.f;
							rtt_min = rtt_samples[0];
							rtt_max = rtt_samples[0];
							for (float sample : rtt_samples) {
								sum += sample;
								rtt_min = std::min(rtt_min, sample);
								rtt_max = std::max(rtt_max, sample);
							}
							rtt_average = sum / rtt_samples.size();
							latency_measured = true;
							Utils::printMsg("Latency measured: RTT avg " + FormatMs(rtt_average)
								+ " (min " + FormatMs(rtt_min) + ", max " + FormatMs(rtt_max) + ")", MessageType::success);
						}
					}
					else {
						Utils::printMsg("Unexpected packet type from the Observer, ignoring it.", warning);
					}
				}

				// Measure the latency: send one ping at a time, until we have enough samples.
				if (is_connected && !latency_measured) {
					if (!ping_in_flight) {
						sf::Packet ping;
						ping << static_cast<std::int32_t>(PacketType::Ping) << next_ping_id;
						outbox.push_back(ping);
						ping_clock.restart();
						ping_in_flight = true;
					}
					else if (ping_clock.getElapsedTime().asSeconds() > ping_timeout) {
						// No reply for too long, so give up on this ping. The next one gets a new number, so a
						// late reply to this one can't be mistaken for it.
						Utils::printMsg("Ping timed out, trying again.", warning);
						ping_in_flight = false;
						next_ping_id++;
					}
				}

				// Don't send anything until we have the Observer's clock, or our timestamps would
				// jump when it arrives.
				if (is_connected && clock_synced) {
					// Send messages only as often as the send rate allows. We keep sending even when the
					// tank isn't moving, because the Observer's prediction needs to hear that it has stopped.
					// If a lot is already queued the connection is struggling, so hold back and send the
					// newest position once it catches up.
					if (send_timer >= send_rate && outbox.size() < max_queued_updates) {
						// Get data structure from game, containing update message.
						TankMessage message = game.GetNetworkUpdate();
						// Add current game time to the message
						message.time = game_time;
						// Translate our messgage struct to sf::Packet (very rudimentary conversion).
						sf::Packet packet;
						packet << static_cast<std::int32_t>(PacketType::Update)
							<< message.time << message.position.x << message.position.y;
						outbox.push_back(packet);
						// Reset timer after queueing the message
						send_timer = 0;
					}
				}

				if (is_connected) {
					// Send what we can. A Partial or NotReady result means the packet at the front of the
					// queue is only part-way out, so it stays queued and is sent again next frame.
					sf::Socket::Status status = FlushOutbox(*player_socket, outbox);
					if (status == sf::Socket::Status::Disconnected || status == sf::Socket::Status::Error)
						lose_connection("Lost connection to Observer.");
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