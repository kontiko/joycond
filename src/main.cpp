#include <map>
#include <chrono>
#include <iostream>
#include <libudev.h>
#include <sys/epoll.h>
#include <vector>

#include "phys_ctlr.h"
#include "virt_ctlr_passthrough.h"

const int MAX_EVENTS = 10;

std::map<std::string, phys_ctlr *> pairing_ctlrs;
std::vector<virt_ctlr *> active_ctlrs;

void add_new_ctlr(struct udev_device *dev, int epoll_fd)
{
    std::string devpath = udev_device_get_devpath(dev);
    std::string devname = udev_device_get_devnode(dev);
    struct epoll_event ctlr_event;

    if (!pairing_ctlrs.count(devpath)) {
        auto phys = new phys_ctlr(devpath, devname);
        std::cout << "Creating new phys_ctlr for " << devname << std::endl;
        pairing_ctlrs[devpath] = phys;
        ctlr_event.events = EPOLLIN;
        ctlr_event.data.fd = phys->get_fd();
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, phys->get_fd(), &ctlr_event)) {
            std::cerr << "Failed to add ctlr_event to epoll; errno=" << errno << std::endl;
            exit(1);
        }
    }
}

int init_pairing_ctlrs(struct udev *udev, int epoll_fd)
{
    struct udev_enumerate *enumerate;
    struct udev_list_entry *devlist;
    struct udev_list_entry *deventry;

    enumerate = udev_enumerate_new(udev);
    if (!enumerate) {
        std::cerr << "Failed to create new udev enumeration\n";
        return 1;
    }
    udev_enumerate_add_match_tag(enumerate, "joycond");
    udev_enumerate_scan_devices(enumerate);
    devlist = udev_enumerate_get_list_entry(enumerate);
    if (devlist) {
        udev_list_entry_foreach(deventry, devlist) {
            char const *path = udev_list_entry_get_name(deventry);
            struct udev_device *dev = udev_device_new_from_syspath(udev, path);
            std::string devpath = udev_device_get_devpath(dev);

            add_new_ctlr(dev, epoll_fd);
            udev_device_unref(dev);
        }
    }
    udev_enumerate_unref(enumerate);

    return 0;
}

void remove_ctlr(std::string const &devpath, int epoll_fd)
{
    struct epoll_event ctlr_event;

    if (pairing_ctlrs.count(devpath)) {
        ctlr_event.events = EPOLLIN;
        ctlr_event.data.fd = pairing_ctlrs[devpath]->get_fd();
        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, pairing_ctlrs[devpath]->get_fd(), &ctlr_event)) {
            std::cerr << "Failed to remove ctlr_event to epoll; errno=" << errno << std::endl;
            exit(1);
        }
        delete pairing_ctlrs[devpath];
        pairing_ctlrs.erase(devpath);
    } else {
        for (int i = 0; i < active_ctlrs.size(); i++) {
            virt_ctlr *virt = active_ctlrs[i];
            if (virt->contains_phys_ctlr(devpath.c_str())) {
                for (auto phys : virt->get_phys_ctlrs()) {
                    if (devpath == phys->get_devpath()) {
                        ctlr_event.events = EPOLLIN;
                        ctlr_event.data.fd = phys->get_fd();
                        if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, phys->get_fd(), &ctlr_event)) {
                            std::cerr << "Failed to remove ctlr_event to epoll; errno=" << errno << std::endl;
                            exit(1);
                        }
                        delete phys;
                    } else {
                        phys->grab();
                        phys->set_all_player_leds(false);
                        pairing_ctlrs[phys->get_devpath()] = phys;
                    }
                }
                delete virt;
                active_ctlrs[i] = nullptr; // free up slot for other ctlr
                break;
            }
        }
    }
}

void udev_event_handler(struct udev_monitor *mon, int epoll_fd)
{
    struct udev_device *dev;

    dev = udev_monitor_receive_device(mon);
    if (dev) {
        std::string action = udev_device_get_action(dev);
        std::string devpath = udev_device_get_devpath(dev);

        std::cout << "DEVNAME="    << udev_device_get_sysname(dev);
        std::cout << " ACTION="    << udev_device_get_action(dev);
        std::cout << " DEVPATH="   << udev_device_get_devpath(dev);
        std::cout << std::endl;

        if (std::string("add") == action && !pairing_ctlrs.count(devpath)) {
            add_new_ctlr(dev, epoll_fd);
        } else if (std::string("remove") == action) {
            remove_ctlr(devpath, epoll_fd);
        }
    }
}

void add_passthrough_ctlr(phys_ctlr *phys)
{
    struct virt_ctlr_passthrough *passthrough;

    passthrough = new virt_ctlr_passthrough(phys);
    phys->set_all_player_leds(false);

    bool found_slot = false;
    for (int i = 0; i < active_ctlrs.size(); i++) {
        if (!active_ctlrs[i]) {
            found_slot = true;
            phys->set_player_led(i % 4, true);
            active_ctlrs[i] = passthrough;
            break;
        }
    }
    if (!found_slot) {
        phys->set_player_led(active_ctlrs.size() % 4, true);
        active_ctlrs.push_back(passthrough);
    }

    pairing_ctlrs.erase(phys->get_devpath());
}

void ctlr_event_handler(int event_fd, int epoll_fd)
{
    for (auto& kv : pairing_ctlrs) {
        struct phys_ctlr *ctlr = kv.second;
        if (event_fd == ctlr->get_fd()) {
            ctlr->handle_events();
            switch (ctlr->get_pairing_state()) {
                case phys_ctlr::PairingState::Lone:
                    std::cout << "Lone controller paired\n";
                    add_passthrough_ctlr(ctlr);
                    break;
                case phys_ctlr::PairingState::Waiting:
                    std::cout << "Waiting controller needs partner\n";
                    break;
                case phys_ctlr::PairingState::Horizontal:
                    std::cout << "Joy-Con paired in horizontal mode\n";
                    add_passthrough_ctlr(ctlr);
                default:
                    break;
            }
            break;
        }
    }

    for (auto& ctlr : active_ctlrs) {
        if (!ctlr)
            continue;
        if (ctlr->contains_fd(event_fd))
            ctlr->handle_events();
    }
}

int main(int argc, char *argv[])
{
    struct udev *udev;
    struct udev_monitor *mon;
    int udev_mon_fd;
    int epoll_fd;
    struct epoll_event udev_mon_event;
    struct epoll_event events[MAX_EVENTS];
    bool pairing_leds_on = false;

    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        std::cerr << "Failed to create epoll; " << epoll_fd << std::endl;
        return 1;
    }

    udev = udev_new();
    if (!udev) {
        std::cerr << "Failed to create udev\n";
        return 1;
    }

    mon = udev_monitor_new_from_netlink(udev, "udev");
    if (!mon) {
        std::cerr << "Failed to create udev monitor\n";
        return 1;
    }
    udev_monitor_filter_add_match_tag(mon, "joycond");
    udev_monitor_enable_receiving(mon);
    udev_mon_fd = udev_monitor_get_fd(mon);

    if (init_pairing_ctlrs(udev, epoll_fd)) {
        std::cerr << "Failed to init pairing controllers list\n";
        return 1;
    }

    udev_mon_event.events = EPOLLIN;
    udev_mon_event.data.fd = udev_mon_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, udev_mon_fd, &udev_mon_event)) {
        std::cerr << "Failed to add udev_mon to epoll; errno=" << errno << std::endl;
        return 1;
    }

    auto last_blink_millis = std::chrono::system_clock::now();
    while (true) {
        int nfds;

        nfds = epoll_pwait(epoll_fd, events, MAX_EVENTS, 50, nullptr);
        if (nfds == -1) {
            std::cerr << "epoll_pwait failure\n";
            return 1;
        }

        for (int i = 0; i < nfds; ++i) {
            if (events[i].data.fd == udev_mon_fd) {
                udev_event_handler(mon, epoll_fd);
            } else {
                ctlr_event_handler(events[i].data.fd, epoll_fd);
            }
        }

        using namespace std::chrono_literals;
        auto current_millis = std::chrono::system_clock::now();
        if (current_millis - last_blink_millis >= 500ms) {
            for (auto& kv : pairing_ctlrs) {
                if (!kv.second->set_all_player_leds(pairing_leds_on))
                    std::cerr << "Failed to set player LEDS\n";
            }
            last_blink_millis = current_millis;
            pairing_leds_on = !pairing_leds_on;
        }
    }

    udev_monitor_unref(mon);
    udev_unref(udev);
    return 0;
}
