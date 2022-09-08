[![license](https://img.shields.io/github/license/sysprogs/PicoHTTPServer?style=flat-square)](https://github.com/sysprogs/PicoHTTPServer)

# PicoHTTPServer: Host responsive Web Apps on Raspberry Pi Pico W

This project turns your Raspberry Pi Pico W into a Wi-Fi access-point. Connecting to it from your computer, tablet or phone will automatically redirect you to a page allowing to control individual pins of the board:

![Pin control page](https://github.com/sysprogs/PicoHTTPServer/raw/master/screenshots/01-pins.png)

Simply click on the **IN** label to use a pin as an input (0/1 will be automatically updated every 500ms), or click on **OUT** to turn it into an output and then click on **0** or **1** to control it.

## Demonstrated Technologies

This project demonstrates how to use Raspberry Pi Pico W to host a web app with responsive CSS, a simple API, modal dialogs and a zero-configuration setup that allows users to open the app by connecting to the Wi-Fi network without having to enter any host names or IP addresses. The project is licensed under the MIT license and can be used as a template to create web apps for managing the configuration of your IoT devices.

The project demonstrates the following technologies:

- The use of DHCP, DNS and HTTP redirects to automatically show the app for all Wi-Fi clients
- A memory-efficient HTTP server capable of handling large requests with very little RAM
- A simple file system for storing multiple files (HTML, images) in the FLASH memory and serving them via HTTP
- A simple CSS layout sufficient to scale the page depending on the browser window size
- A simple API showing how to fetch meaningful data directly from the C code and present it in a meaningful way

### "Sign into Network" Message

This feature relies on the mechanism used by public networks (e.g. airport/hotel) to authenticate users, or make them accept the terms and conditions. When a client connects to such a network, it typically:

1. Assigns the client a private DHCP address (e.g. 192.168.0.100) and points it to a local DNS server.
2. When the client tries to resolve a hostname (e.g. `sysprogs.com`, it is resolved to the correct IP address).
3. When the client tries to connect to that address, the router intercepts the request and issues an HTTP 302 redirect to the login page.

Most modern browsers and operating systems support this behavior out-of-the-box by trying to connect to a test URL (e.g. http://www.msftconnecttest.com/redirect). If the connection results in a HTTP redirect, the operating system suggests opening the browser to login:

![Automatic sign-into-network message](https://github.com/sysprogs/PicoHTTPServer/raw/master/screenshots/02-login.png)

This mechanism is known as [Captive Portals](https://en.wikipedia.org/wiki/Captive_portal) and is implemented by PicoHTTPServer as follows:

1. The [DHCP server](https://github.com/sysprogs/PicoHTTPServer/blob/master/PicoHTTPServer/dhcpserver/dhcpserver.c) (taken from the [access point](https://github.com/raspberrypi/pico-examples/tree/master/pico_w/access_point/dhcpserver) example) issues the clients IP addresses from the pre-configured subnet, and reports itself as the gateway and DNS server (see `DHCP_OPT_ROUTER` and `DHCP_OPT_DNS`).

2. The [DNS server](https://github.com/sysprogs/PicoHTTPServer/blob/master/PicoHTTPServer/dns/dnsserver.c) compares the requested domain names to the configured name of the Pico W (e.g. `picohttp.piconet.local`). If the name matches, it returns the IP address of the Pico W itself. Note that if we return any private address when the client OS is testing the connection (e.g. resolving http://www.msftconnecttest.com/redirect), most OSes will conclude that it's a private network and won't show the login prompt. To work around it, we resolve all hostnames that don't match our own name to the **secondary IP** (the default configuration sets it to [TEST-NET-2](https://en.wikipedia.org/wiki/Reserved_IP_addresses) that is 198.51.100.0).

3. In order to answer requests to the **secondary IP**, we use a [patched version of lwIP](https://github.com/sysprogs/PicoHTTPServer/blob/master/lwip_patch/lwip.patch) that it routes packets with this IP address to our netconn instance. From the client's perspective, this is similar to a network router.

4. Finally, the HTTP server checks the `Host` field in the HTTP request. If the request came for our hostname (e.g. `picohttp.piconet.local`), it is handled normally. If not, it issues an `HTTP/1.0 302 Found` redirect pointing to the primary hostname.


### A Memory-efficient HTTP Server

As the Raspberry Pi Pico W only has 256KB of RAM, this project uses its own memory-efficient implementation of the [HTTP server](https://github.com/sysprogs/PicoHTTPServer/blob/master/PicoHTTPServer/httpserver.c) designed with the following constraints in mind:

- The entire HTTP request never needs to fit into the RAM. The server (see `parse_and_handle_http_request()`) reads and parses the request using a small memory window (4KB by default). The window needs to be big enough to fit the first line (method + path) and the **longest** line from the request header that we want to parse. All request headers that don't fit into the window (e.g. a very long `User-Agent` field) will be safely skipped without interfering with the rest of the fields.

- Likewise, the application logic can generate API responses using the printf()-style `http_server_write_reply()` function without having to fit the entire response into the buffer. As long as a single fragment formatted at once fits, the server will manage the buffering automatically.

- All data sent by the web app via POST requests can be read line-by-line without having to fit the entire request in memory.

This architecture allows handling HTTP requests at decent speeds with only 4KB/thread (+2KB default stack) that can be reduced further at some performance cost.

### A Simple File System

In order to support images, styles or multiple pages, the HTTP server includes a tool packing the served content into a single file (along with the content type for each file). The file is then embedded into the image, and is programmed together with the rest of the firmware. You can easily add more files to the web server by simply putting them into the [www](https://github.com/sysprogs/PicoHTTPServer/tree/master/PicoHTTPServer/www) directory and rebuilding the project with CMake.

You can dramatically reduce the FLASH utilization by the web server content by pre-compressing the files with gzip and returning the `Content-Encoding: gzip` header for the affected files. The decompression will happen on the browser side, without the need to include decompression code in the firmware.

### The Web App

Raspberry Pi Pico W has 2MB of FLASH memory (~256KB of which are used by the Pico SDK), so it cannot fit a full-scale web framework, or a PHP interpreter. However, it can easily serve JavaScript that will execute in the browser, making calls to various APIs and updating the page accordingly. This is demonstrated by the settings editing popup:

![Settings editing GUI](https://github.com/sysprogs/PicoHTTPServer/raw/master/screenshots/03-settings.png)

Whenever you click the 'settings' button in the browser, the following events take place:

1. The JavaScript uses the `XMLHttpRequest` interface to send a GET request to the `/api/settings` endpoint. The code in `do_handle_api_call` in `main.c` handles this request, formatting the settings as a JSON object using `http_server_write_reply()`.
2. The JavaScript parses the reply and sets the fields in the settings popup. Note that the JSON parsing is done in the browser, so the code running on Raspberry Pi Pico doesn't need to handle it.
3. When the user clicks the 'OK' button in the browser, the JavaScript formats the settings fields into a set of `key=value` lines and sends it as a POST request.
4. The code in `parse_server_settings()` reads and validates the values from the browser. Because the values are sent in plain text, the entire request doesn't need to fit into the memory at the same time. Instead, the code can read it line-by-line using `http_server_read_post_line()`.

The settings are stored in the FLASH memory together with the firmware and the web pages, so they are preserved when you reboot the device.

## Building the App

You can download the pre-built binary of the HTTP server from the [releases](https://github.com/sysprogs/PicoHTTPServer/releases) page. Simply boot your Raspberry Pi Pico W into the bootloader, and copy the **PicoHTTPServer.uf2** file on it. The Pico W will restart and create the **PicoHTTP** network.

The easiest way to build the sources on Windows is to install [VisualGDB](https://visualgdb.com/) and open the `PicoHTTPServer.sln` file in Visual Studio. VisualGDB will automatically install the necessary toolchain/SDK and will manage build and debugging for you.

You can also build the project manually by running the [build-all.sh](https://github.com/sysprogs/PicoHTTPServer/blob/master/build-all.sh) file. Make sure you have CMake and GNU Make installed, and that you have the ARM GCC (arm-none-eabi) in the PATH.

## Modifying the App

See [this tutorial](https://visualgdb.com/tutorials/raspberry/pico_w/http/) for detailed step-by-step instructions on adding a new dialog and the corresponding API to the app, as well as testing it out on the hardware.