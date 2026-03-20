#include <atomic>
#include <chrono>
#include <csignal>
#include <exception>
#include <ext/stdio_filebuf.h>
#include <iostream>
#include <list>
#include <mutex>
#include <netinet/in.h>
#include <stdio.h>
#include <string>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <thread>

#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using json = nlohmann::json;

static std::string wsdata;
static std::string windata;
static std::string wsactive;
static std::string winactive;
static std::string backup;

static const auto loc = std::locale("fi_FI.UTF-8");
static const auto zone = std::chrono::current_zone();

std::string center = "";
std::mutex mtx;
std::atomic_bool die{false};

bool socket_write(int socket, std::string msg)
{
    size_t n = 0;
    while (n < msg.size())
    {
        ssize_t sent = send(socket, msg.data() + n, msg.size() - n, 0);
        if (sent == -1)
            return false;
        n += sent;
    }
    // std::cerr << "sent " << msg << std::endl;
    return true;
}

std::string socket_recv(int socket)
{
    std::vector<char> data;
    char buffer[1024];
    while (!die)
    {
        ssize_t n = recv(socket, buffer, sizeof(buffer), 0);
        if (n > 0)
        {
            data.insert(data.end(), buffer, buffer + n);
        }
        else if (n == 0)
        {
            break;
        }
        else
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                break;
            return {};
        }
    }
    auto reply = std::string{data.begin(), data.end()};
    // std::cerr << "recv " << reply << std::endl;
    return reply;
}

std::string socket_read(int socket)
{
    std::vector<char> data;
    char buffer[1024];
    while (!die)
    {
        ssize_t n = read(socket, buffer, sizeof(buffer));
        if (n > 0)
        {
            data.insert(data.end(), buffer, buffer + n);
        }
        else if (n == 0)
        {
            break;
        }
        else
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                break;
            return {};
        }
    }
    auto reply = std::string{data.begin(), data.end()};
    // std::cerr << "recv " << reply << std::endl;
    return reply;
}

std::string ipc_send(std::string msg)
{
    const auto path = std::string{getenv("XDG_RUNTIME_DIR")} + "/hypr/" + std::string{getenv("HYPRLAND_INSTANCE_SIGNATURE")} + "/.socket.sock";
    auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd == -1)
    {
        std::cerr << "Error: send: no socket" << std::endl;
        return "";
    }
    sockaddr_un addr{.sun_family = AF_UNIX};
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == -1)
    {
        std::cerr << "Error: send: no connect" << std::endl;
        close(fd);
        return "";
    }
    socket_write(fd, msg);
    std::string reply = socket_recv(fd);
    close(fd);
    return reply;
}

struct SWindow
{
    std::string address;
    int workspace_id;
    bool active;
    int x;
    int y;
};

struct SWorkspace
{
    int id;
    std::string name;
    bool active;
    std::list<SWindow> windows;
};
std::list<SWorkspace> wss;

std::string workspaces()
{
    std::string out;
    wss.sort(
        [](SWorkspace& a, SWorkspace& b)
        {
            if (!a.name.starts_with("special:") && b.name.starts_with("special:"))
                return true;
            if (!b.name.starts_with("special:") && a.name.starts_with("special:"))
                return false;
            int aid = __INT_MAX__;
            int bid = __INT_MAX__;
            try
            {
                aid = std::stoi(a.name);
            }
            catch (...)
            {
            };
            try
            {
                bid = std::stoi(b.name);
            }
            catch (...)
            {
            };
            if (aid != bid)
                return aid < bid;
            return (a.name.starts_with("special:") ? a.name[8] : a.name[0]) < (b.name.starts_with("special:") ? b.name[8] : b.name[0]);
        });
    for (auto& ws : wss)
    {
        ws.windows.sort([](SWindow& a, SWindow& b) { return a.x < b.x || a.y < b.y; });
    }
    for (auto& ws : wss)
    {
        if (ws.name.starts_with("special:"))
        {
            if (ws.active)
                out += "<span color='#ff69b4ff' letter_spacing='3072'>";
            else
                out += "<span color='#ff555555' letter_spacing='3072'>";
        }
        else
        {
            if (ws.active)
                out += "<span color='#ffffffff' letter_spacing='3072'>";
            else
                out += "<span color='#ffffff55' letter_spacing='3072'>";
        }
        if (ws.id > 0)
            out += std::format("<span letter_spacing='0'>{}</span>", ws.id);
        else
            out += std::format("{:c}", ws.name.starts_with("special:") ? std::toupper(ws.name[8]) : std::toupper(ws.name[0]));
        out += "</span><span baseline_shift='1.5pt' letter_spacing='1024'>";
        for (auto& win : ws.windows)
        {
            if (win.active)
                out += "<span color='#ffffffff'>●</span>";
            else
                out += "<span color='#ffffff33'>●</span>"; // ○
        }
        out += "</span><span color='#ffffffff' letter_spacing='-4096'> </span>";
    }
    if (!out.empty())
        backup = out;
    return out;
}

void draw_workspaces()
{
    mtx.lock();
    std::cout << "1,<span color='#ffffffff'>" + workspaces() + "</span>" << std::endl;
    mtx.unlock();
}

void draw_pipe()
{
    std::string line;
    /*struct pollfd fds;
    int ret;
    fds.fd = 0;
    fds.events = POLLIN;
    ret = poll(&fds, 1, 0);
    while (ret && std::getline(std::cin, line))*/
    while (std::getline(std::cin, line))
    {
        mtx.lock();
        center = line;
        std::cout << "2," << center << std::endl;
        mtx.unlock();
    }
}

void draw_clock()
{
    mtx.lock();
    auto now = std::chrono::zoned_time{zone, std::chrono::system_clock::now()};
    // who the fuck designs these format libs so it's impossible to get fucking numbers without leading zeros
    int day = std::stoi(std::format(loc, "{:L%d}", now));
    int mon = std::stoi(std::format(loc, "{:L%m}", now));
    std::cout << "3,<span color='#ffffffff'>" + std::format(loc, "#{:L%OV %a} {}.{}. {:L%H:%M:%OS}", now, day, mon, now) + "</span>" << std::endl;
    mtx.unlock();
}

void draw_all()
{
    draw_workspaces();
    mtx.lock();
    std::cout << "2," << center << std::endl;
    mtx.unlock();
    draw_clock();
}

void ipc_update()
{
    wsdata = ipc_send("j/workspaces");
    windata = ipc_send("j/clients");
    wsactive = ipc_send("j/activeworkspace");
    winactive = ipc_send("j/activewindow");

    json window;
    json windows;
    json workspace;
    json workspaces;

    try
    {
        window = json::parse(winactive);
        windows = json::parse(windata);
        workspace = json::parse(wsactive);
        workspaces = json::parse(wsdata);
    }
    catch (const json::exception& e)
    {
        std::cerr << "Parse error: " << e.what() << std::endl;
        return;
    }

    wss.clear();

    for (auto& ws : workspaces)
    {
        auto id = ws["id"].get<int>();
        auto name = ws["name"].get<std::string>();
        SWorkspace nws{id, name, workspace["id"] == id};
        wss.emplace_back(std::move(nws));
    }
    for (auto& win : windows)
    {
        auto addr = win["address"].get<std::string>();
        auto id = win["workspace"]["id"].get<int>();
        auto x = win["at"][0].get<int>();
        auto y = win["at"][1].get<int>();
        SWindow nwin{addr, id, window["address"] == addr, x, y};
        for (auto& ws : wss)
        {
            if (ws.id != id)
                continue;
            ws.windows.emplace_back(std::move(nwin));
        }
    }
}

bool ipc_handle(std::string event)
{
    auto data = event.substr(event.find_last_of('>') + 1);
    if (event.starts_with("openwindow>>"))
    {
        ipc_update();
        return true;
    }
    else if (event.starts_with("closewindow>>"))
    {
        for (auto& ws : wss)
        {
            auto it = std::find_if(ws.windows.begin(), ws.windows.end(), [&data](SWindow& win) { return win.address == "0x" + data; });
            if (it != ws.windows.end())
            {
                ws.windows.erase(it);
                return true;
            }
        }
    }
    else if (event.starts_with("activewindowv2>>"))
    {
        for (auto& ws : wss)
        {
            ws.active = false;
            for (auto& win : ws.windows)
            {
                if (win.address == "0x" + data)
                {
                    ws.active = true;
                    win.active = true;
                }
                else
                {
                    win.active = false;
                }
            }
        }
        return true;
    }
    else if (event.starts_with("focusedmonv2>>"))
    {
        try
        {
            auto id = std::stoi(data.substr(data.find_first_of(',') + 1));
            for (auto& ws : wss)
            {
                if (ws.id == id)
                    ws.active = true;
                else
                    ws.active = false;
            }
        }
        catch (std::exception e)
        {
        }
        return true;
    }
    else if (event.starts_with("workspacev2>>"))
    {
        try
        {
            auto id = std::stoi(data.substr(0, data.find_first_of(',')));
            bool found = false;
            for (auto& ws : wss)
            {
                if (ws.id == id)
                {
                    ws.active = true;
                    found = true;
                }
                else
                    ws.active = false;
            }
            if (!found)
                ipc_update();
        }
        catch (std::exception e)
        {
        }
        return true;
    }
    else if (event.starts_with("destroyworkspacev2>>"))
    {
        try
        {
            auto id = std::stoi(data.substr(0, data.find_first_of(',')));
            auto it = std::find_if(wss.begin(), wss.end(), [&id](SWorkspace& ws) { return ws.id == id; });
            if (it != wss.end())
            {
                wss.erase(it);
                return true;
            }
        }
        catch (std::exception e)
        {
        }
    }

    return false;
}

bool has_data(int sockfd)
{
    // 1. Initialize an fd_set for reading
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    // 2. Set a timeout value of zero for immediate polling (non-blocking)
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;

    // 3. Call select() to monitor the socket for readiness to read
    // The first argument (nfds) should be the highest file descriptor + 1.
    int ready_count = select(sockfd + 1, &readfds, NULL, NULL, &tv);

    // 4. Check the result of select()
    if (ready_count == -1)
    {
        // Handle error (e.g., check errno)
        perror("select error");
        return false;
    }
    else if (ready_count == 0)
    {
        // No data available within the timeout (which is 0)
        return false;
    }
    else
    {
        // ready_count > 0: Check if our specific socket is in the set
        if (FD_ISSET(sockfd, &readfds))
        {
            // Optional: You can use ioctl(FIONREAD) to get the exact number of bytes waiting
            // int bytes_available;
            // ioctl(sockfd, FIONREAD, &bytes_available);
            // std::cout << bytes_available << " bytes available." << std::endl;

            return true; // Data is available
        }
        return false;
    }
}

void ipc_listen()
{
    while (!die)
    {
        const auto path = std::string{getenv("XDG_RUNTIME_DIR")} + "/hypr/" + std::string{getenv("HYPRLAND_INSTANCE_SIGNATURE")} + "/.socket2.sock";
        auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd == -1)
        {
            std::cerr << "Error: listen: no socket" << std::endl;
            continue;
        }
        sockaddr_un addr{.sun_family = AF_UNIX};
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
        if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == -1)
        {
            std::cerr << "Error: listen: no connect" << std::endl;
            close(fd);
            continue;
        }
        __gnu_cxx::stdio_filebuf<char> filebuf(fd, std::ios::in);
        std::istream is(&filebuf);
        std::string line;
        while (!die && std::getline(is, line))
        {
            if (ipc_handle(line))
                draw_workspaces();
        }
        close(fd);
    }
}

void handler(int n)
{ die = true; }

int main()
{
    signal(SIGTERM, handler);
    signal(SIGPIPE, handler);
    ipc_update();
    std::thread t(ipc_listen);
    t.detach();
    std::thread t2(draw_pipe);
    t2.detach();
    draw_all();
    while (!die)
    {
        draw_clock();
        usleep(1000000);
    }
    return 0;
}
