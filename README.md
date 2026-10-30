CMP425 / CMP501 
Lab 6: position interpolation example - by Andrei Boiko
Built using SFML 3

### Key changes from Lab 5
- Updated connecton state handling:
	- Setting receiving socket to non-blocking mode to avoid unexpected blocking recieve call.
	- Resetting all connection states to NotReady before checking for updates with Selector. This ensures a socket won't be considered Ready unless wait() reported it.
	- Selector with a 0 seconds timeout actually waits indefinitely until at least one socket is ready. So changed this to 5ms to make it return almost immediately and not block.
  
- Updated connection struct:
	- Now storing last 3 positional updates to allow interpolation (2 currently used for interpolation, more can be used for prediction if needed).
	- Added Interpolate method to calculate interpolated position based on last 2 updates and time between those messages.
	- Addeed prediction method enum to allow different prediction methods in future.


### Player Mode
Attempt to connect to the observer with default timeout (1 second). 
When connected, send positional data using blocking send calls. 

### Observer Mode
Accept multiple connections from players and handle updates using socket selector.
Draw player using intrpolated positions based on last 2 updates.
