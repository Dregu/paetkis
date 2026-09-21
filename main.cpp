#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <ext/stdio_filebuf.h>
#include <filesystem>
#include <format>
#include <fstream>
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
#include <vector>

#include <nlohmann/json.hpp>

using namespace std::chrono_literals;
using json = nlohmann::json;

static std::string wsdata;
static std::string windata;
static std::string wsactive;
static std::string winactive;
static std::string mondata;
static std::string backup;

static const auto loc = std::locale("fi_FI.UTF-8");
static const auto zone = std::chrono::current_zone();

std::string center = "";
std::mutex mtx;
std::atomic_bool die{false};

namespace fs = std::filesystem;

struct Battery
{
    int percentage = -1;
    std::string state = "Unknown";
    double hours = -1.0;
};

Battery get_battery()
{
    Battery total;

    double total_now = 0.0;
    double total_full = 0.0;
    double total_rate = 0.0;

    bool is_charging = false;
    bool is_discharging = false;
    bool is_full = true;
    bool found_battery = false;

    std::string path = "/sys/class/power_supply/";
    if (!fs::exists(path))
        return total;

    for (const auto& entry : fs::directory_iterator(path))
    {
        std::string name = entry.path().filename().string();

        if (name.rfind("BAT", 0) == 0)
        {
            found_battery = true;
            std::string base = entry.path().string() + "/";

            std::ifstream f_now(base + "energy_now");
            std::ifstream f_rate(base + "power_now");
            std::ifstream f_full(base + "energy_full");

            if (!f_now.is_open())
            {
                f_now.open(base + "charge_now");
                f_rate.open(base + "current_now");
                f_full.open(base + "charge_full");
            }

            double b_now = 0, b_rate = 0, b_full = 0;
            if (f_now >> b_now && f_rate >> b_rate && f_full >> b_full)
            {
                total_now += b_now;
                total_full += b_full;
                total_rate += b_rate;
            }

            std::ifstream f_status(base + "status");
            std::string b_state;
            if (std::getline(f_status, b_state))
            {
                if (b_state == "Charging")
                    is_charging = true;
                if (b_state == "Discharging")
                    is_discharging = true;
                if (b_state != "Full")
                    is_full = false;
            }
        }
    }

    if (!found_battery || total_full == 0.0)
        return total;

    total.percentage = static_cast<int>(std::round((total_now / total_full) * 100.0));

    if (is_charging)
        total.state = "Charging";
    else if (is_discharging)
        total.state = "Discharging";
    else if (is_full)
        total.state = "Full";
    else
        total.state = "Not Charging";

    if (total_rate > 0.0)
    {
        if (total.state == "Discharging")
        {
            total.hours = total_now / total_rate;
        }
        else if (total.state == "Charging")
        {
            total.hours = (total_full - total_now) / total_rate;
        }
    }

    return total;
}

std::string format_battery(Battery& bat)
{
    if (bat.percentage == -1)
        return {};
    std::string time;
    std::string icon;
    if (bat.hours >= 0.0)
    {
        int hours = static_cast<int>(bat.hours);
        int minutes = static_cast<int>(std::round((bat.hours - hours) * 60));
        if (minutes == 60)
        {
            hours = 1;
            minutes = 0;
        }
        time = std::format("{}:{:02}", hours, minutes);
    }
    size_t index = static_cast<size_t>(std::clamp(bat.percentage / 10, 0, 10));
    static const std::vector<std::string> ICONS = {"󰂎", "󰁺", "󰁻", "󰁼", "󰁽", "󰁾", "󰁿", "󰂀", "󰂁", "󰂂", "󰁹"};
    icon = std::format("{}{}", ICONS[index], bat.state == "Charging" ? "" : "");
    return std::format("{} {}% {}   ", icon, bat.percentage, time);
}

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

bool has_data(int sockfd)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    return select(sockfd + 1, &readfds, NULL, NULL, &tv) > 0 && FD_ISSET(sockfd, &readfds);
}

struct SWindow
{
    std::string address;
    bool active;
    int x;
    int y;
};

struct SWorkspace
{
    std::string address;
    std::string type;
    std::string name;
    bool active;
    std::list<SWindow> windows;

    std::string id()
    {
        if (name.empty())
            return "?";
        size_t len = 1;
        unsigned char c = static_cast<unsigned char>(name[0]);
        if (c < 0x80)
            len = 1;
        else if ((c & 0xE0) == 0xC0)
            len = 2;
        else if ((c & 0xF0) == 0xE0)
            len = 3;
        else if ((c & 0xF8) == 0xF0)
            len = 4;
        else
            len = 1;
        auto n = name.substr(0, len);
        std::ranges::transform(n, n.begin(), [](unsigned char c) { return std::toupper(c); });
        return n;
    }
};
std::list<SWorkspace> wss;

std::string workspaces()
{
    std::string out;
    wss.sort(
        [](SWorkspace& a, SWorkspace& b)
        {
            if (a.type != "special" && b.type == "special")
                return true;
            if (b.type != "special" && a.type == "special")
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
            return a.name[0] < b.name[0];
        });
    for (auto& ws : wss)
    {
        ws.windows.sort([](SWindow& a, SWindow& b) { return a.x < b.x || a.y < b.y; });
    }
    for (auto& ws : wss)
    {
        if (ws.type == "special")
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
        try
        {
            auto id = std::stoi(ws.address);
            out += std::format("<span letter_spacing='0'>{}</span>", id);
        }
        catch (...)
        {
            out += ws.id();
        }
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
    if (!has_data(0))
        return;
    std::string line;
    while (!die && std::getline(std::cin, line))
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
    int week = std::stoi(std::format(loc, "{:L%V}", now));

    auto bat = get_battery();
    std::cout << "3,<span color='#ffffffff'>" + format_battery(bat) + std::format(loc, "#{1} {0:L%a} {2}.{3}. {0:L%H:%M:%OS}", now, week, day, mon) + "</span>"
              << std::endl;
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
    mondata = ipc_send("j/monitors");

    json window;
    json windows;
    json workspace;
    json workspaces;
    json monitors;

    try
    {
        window = json::parse(winactive);
        windows = json::parse(windata);
        workspace = json::parse(wsactive);
        workspaces = json::parse(wsdata);
        monitors = json::parse(mondata);
    }
    catch (const json::exception& e)
    {
        std::cerr << "Parse error: " << e.what() << std::endl;
        return;
    }

    wss.clear();

    for (auto& ws : workspaces)
    {
        auto addr = ws["address"].get<std::string>();
        auto type = ws["type"].get<std::string>();
        auto name = ws["name"].get<std::string>();
        if (name.starts_with("special:") || name.starts_with("name:"))
            name = name.substr(name.find_first_of(":") + 1);
        SWorkspace nws{addr, type, name, window["workspace"]["address"] == addr || workspace["address"] == addr};
        wss.emplace_back(std::move(nws));
    }
    for (auto& win : windows)
    {
        auto addr = win["address"].get<std::string>();
        auto ws_addr = win["workspace"]["address"].get<std::string>();
        auto x = win["at"][0].get<int>();
        auto y = win["at"][1].get<int>();
        SWindow nwin{addr, window["address"] == addr, x, y};
        for (auto& ws : wss)
        {
            if (ws.address != ws_addr)
                continue;
            ws.windows.emplace_back(std::move(nwin));
        }
    }
    for (auto& mon : monitors)
    {
        auto special = mon["specialWorkspace"]["address"].get<std::string>();
        if (special.empty())
            continue;
        if (auto it = std::ranges::find(wss, special, &SWorkspace::address); it != wss.end())
            it->active = true;
    }
}

bool ipc_handle(std::string event)
{
    auto data = event.substr(event.find_last_of('>') + 1);
    if (event.starts_with("openwindow>>") || event.starts_with("movewindow>>"))
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
            auto addr = data.substr(data.find_first_of(',') + 1);
            for (auto& ws : wss)
            {
                if (ws.address == addr)
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
    else if (event.starts_with("workspacev2>>") || event.starts_with("activespecialv2>>"))
    {
        try
        {
            auto addr = data.substr(0, data.find_first_of(','));
            if (addr.starts_with("name:"))
                addr = addr.substr(5);
            bool found = false;
            for (auto& ws : wss)
            {
                if (ws.address == addr)
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
            auto addr = data.substr(0, data.find_first_of(','));
            if (addr.starts_with("name:"))
                addr = addr.substr(5);
            auto it = std::find_if(wss.begin(), wss.end(), [&addr](SWorkspace& ws) { return ws.address == addr; });
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
    else if (event.starts_with("changeworkspaceid>>"))
    {
        try
        {
            auto addr = data.substr(0, data.find_first_of(','));
            auto to = data.substr(data.find_first_of(',') + 1);
            auto it = std::find_if(wss.begin(), wss.end(), [&addr](SWorkspace& ws) { return ws.address == addr; });
            if (it != wss.end())
            {
                it->address = to;
                return true;
            }
        }
        catch (std::exception e)
        {
        }
    }

    return false;
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
