#include "virt_ctlr_combined.h"

#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <libevdev/libevdev-uinput.h>
#include <linux/uinput.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

//private
void virt_ctlr_combined::relay_events(std::shared_ptr<phys_ctlr> phys)
{
    struct input_event ev;
    struct libevdev *evdev = phys->get_evdev();

    int ret = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    while (ret == LIBEVDEV_READ_STATUS_SYNC || ret == LIBEVDEV_READ_STATUS_SUCCESS) {
        if (ret == LIBEVDEV_READ_STATUS_SYNC) {
            std::cout << "handle sync\n";
            while (ret == LIBEVDEV_READ_STATUS_SYNC) {
                libevdev_uinput_write_event(uidev, ev.type, ev.code, ev.value);
                ret = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_SYNC, &ev);
            }
        } else if (ret == LIBEVDEV_READ_STATUS_SUCCESS) {
            /* First filter out the SL and SR buttons on each physical controller */
            if (phys == physl && ev.type == EV_KEY && (ev.code == BTN_TR || ev.code == BTN_TR2)) {
                ret = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                continue;
            } else if (phys == physr && ev.type == EV_KEY && (ev.code == BTN_TL || ev.code == BTN_TL2)) {
                ret = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                continue;
            }
            libevdev_uinput_write_event(uidev, ev.type, ev.code, ev.value);
        }
        ret = libevdev_next_event(evdev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
    }
}

void virt_ctlr_combined::handle_uinput_event()
{
    struct input_event ev;
    int ret;

    while ((ret = read(get_uinput_fd(), &ev, sizeof(ev))) == sizeof(ev)) {
        switch (ev.type) {
            case EV_FF:
                {
                    struct input_event redirectedl = ev;
                    struct input_event redirectedr = ev;

                    if (!rumble_effects.count(ev.code)) {
                        if (ev.code < FF_GAIN)
                            std::cerr << "ERROR: ff_effect with id=" << ev.code << " is not in map\n";
                    } else {
                        redirectedl.code = rumble_effects[ev.code].first.id;
                        redirectedr.code = rumble_effects[ev.code].second.id;
                    }

                    /* Just forward this FF event on to the actual devices */
                    if (physl) {
                        if (write(physl->get_fd(), &redirectedl, sizeof(redirectedl)) != sizeof(redirectedl))
                            std::cerr << "Failed to forward EV_FF to physl\n";
                    }
                    if (physr) {
                        if (write(physr->get_fd(), &redirectedr, sizeof(redirectedr)) != sizeof(redirectedr))
                            std::cerr << "Failed to forward EV_FF to physr\n";
                    }
                    break;
                }

            case EV_UINPUT:
                switch (ev.code) {
                    case UI_FF_UPLOAD:
                        {
                            struct uinput_ff_upload upload = { 0 };
                            struct ff_effect effect = { 0 };
                            struct ff_effect effect_l = { 0 };
                            struct ff_effect effect_r = { 0 };

                            upload.request_id = ev.value;
                            if (ioctl(get_uinput_fd(), UI_BEGIN_FF_UPLOAD, &upload))
                                std::cerr << "Failed to get uinput_ff_upload: " << strerror(errno) << std::endl;

                            effect = upload.effect;
                            effect.id = -1;
                            /* upload the effect to both devices */
                            upload.retval = 0;
                            if (physl) {
                                if (ioctl(physl->get_fd(), EVIOCSFF, &effect) == -1)
                                    upload.retval = errno;
                                effect_l = effect;
                            }

                            if (physr) {
                                /* reset effect */
                                effect = upload.effect;
                                effect.id = -1;
                                if (ioctl(physr->get_fd(), EVIOCSFF, &effect) == -1)
                                    upload.retval = errno;
                                effect_r = effect;
                            }

                            upload.effect = effect;

                            if (upload.retval)
                                std::cerr << "UI_FF_UPLOAD failed: " << strerror(upload.retval) << std::endl;

                            if (rumble_effects.count(effect.id))
                                std::cerr << "WARNING: ff_effect already in map\n";
                            rumble_effects[effect.id] = std::make_pair(effect_l, effect_r);

                            if (ioctl(get_uinput_fd(), UI_END_FF_UPLOAD, &upload))
                                std::cerr << "Failed to end uinput_ff_upload: " << strerror(errno) << std::endl;

                            break;
                        }
                    case UI_FF_ERASE:
                        {
                            struct uinput_ff_erase erase = { 0 };

                            erase.request_id = ev.value;
                            if (ioctl(get_uinput_fd(), UI_BEGIN_FF_ERASE, &erase))
                                std::cerr << "Failed to get uinput_ff_erase: " << strerror(errno) << std::endl;

                            erase.retval = 0;
                            if (physl) {
                                if (ioctl(physl->get_fd(), EVIOCRMFF, erase.effect_id) == -1)
                                    erase.retval = errno;
                            }
                            if (physr) {
                                if (ioctl(physr->get_fd(), EVIOCRMFF, erase.effect_id) == -1)
                                    erase.retval = errno;
                            }

                            if (erase.retval)
                                std::cerr << "UI_FF_ERASE failed: " << strerror(erase.retval) << std::endl;
                            else if (!rumble_effects.count(erase.effect_id))
                                std::cerr << "WARNING: effect_id not in rumble_effects map\n";
                            else
                                rumble_effects.erase(erase.effect_id);

                            if (ioctl(get_uinput_fd(), UI_END_FF_ERASE, &erase))
                                std::cerr << "Failed to end uinput_ff_erase: " << strerror(errno) << std::endl;

                            break;
                        }
                    default:
                        std::cerr << "Unhandled EV_UNINPUT code=" << ev.code <<std::endl;
                        break;
                }
                break;

            default:
                std::cerr << "Unhandled uinput type=" << ev.type << std::endl;
                break;
        }
    }
    if (ret < 0 && errno != EAGAIN) {
        std::cerr << "Failed reading uinput fd; ret=" << strerror(errno) << std::endl;
    } else if (ret > 0) {
        std::cerr << "Uinput incorrect read size of " << ret << std::endl;
    }
}

//public
virt_ctlr_combined::virt_ctlr_combined(std::shared_ptr<phys_ctlr> physl, std::shared_ptr<phys_ctlr> physr, epoll_mgr& epoll_manager) :
    physl(physl),
    physr(physr),
    epoll_manager(epoll_manager),
    subscriber(nullptr),
    virt_evdev(nullptr),
    uidev(nullptr),
    uifd(-1),
    rumble_effects()
{
    int ret;

    uifd = open("/dev/uinput", O_RDWR);
    if (uidev < 0) {
        std::cerr << "Failed to open uinput; errno=" << errno << std::endl;
        exit(1);
    }

    // Create a virtual evdev on which the uinput will be based
    virt_evdev = libevdev_new();
    if (!virt_evdev) {
        std::cerr << "Failed to create virtual evdev\n";
        exit(1);
    }

    libevdev_set_name(virt_evdev, "Nintendo Switch Combined Joy-Cons");

    // Make sure that all of this configuration remains in sync with the hid-nintendo driver.
    libevdev_enable_event_type(virt_evdev, EV_KEY);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_SELECT, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_Z, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_THUMBL, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_START, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_MODE, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_THUMBR, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_SOUTH, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_EAST, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_NORTH, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_WEST, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_DPAD_UP, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_DPAD_DOWN, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_DPAD_LEFT, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_DPAD_RIGHT, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_TL, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_TR, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_TL2, NULL);
    libevdev_enable_event_code(virt_evdev, EV_KEY, BTN_TR2, NULL);

    struct input_absinfo absconfig = { 0 };
    absconfig.minimum = -32767;
    absconfig.maximum = 32767;
    absconfig.fuzz = 250;
    absconfig.flat = 500;
    libevdev_enable_event_type(virt_evdev, EV_ABS);
    libevdev_enable_event_code(virt_evdev, EV_ABS, ABS_X, &absconfig);
    libevdev_enable_event_code(virt_evdev, EV_ABS, ABS_Y, &absconfig);
    libevdev_enable_event_code(virt_evdev, EV_ABS, ABS_RX, &absconfig);
    libevdev_enable_event_code(virt_evdev, EV_ABS, ABS_RY, &absconfig);

    libevdev_enable_event_type(virt_evdev, EV_FF);
    libevdev_enable_event_code(virt_evdev, EV_FF, FF_RUMBLE, NULL);
    libevdev_enable_event_code(virt_evdev, EV_FF, FF_PERIODIC, NULL);
    libevdev_enable_event_code(virt_evdev, EV_FF, FF_SQUARE, NULL);
    libevdev_enable_event_code(virt_evdev, EV_FF, FF_TRIANGLE, NULL);
    libevdev_enable_event_code(virt_evdev, EV_FF, FF_SINE, NULL);
    libevdev_enable_event_code(virt_evdev, EV_FF, FF_GAIN, NULL);

    // Set the product information to a left joy-con's product info (but with virtual bus type)
    libevdev_set_id_vendor(virt_evdev, 0x57e);
    libevdev_set_id_product(virt_evdev, 0x2006);
    libevdev_set_id_bustype(virt_evdev, BUS_VIRTUAL);
    libevdev_set_id_version(virt_evdev, 0x0000);

    ret = libevdev_uinput_create_from_device(virt_evdev, LIBEVDEV_UINPUT_OPEN_MANAGED, &uidev);
    if (ret) {
        std::cerr << "Failed to create libevdev_uinput; " << ret << std::endl;
        exit(1);
    }

    int flags = fcntl(get_uinput_fd(), F_GETFL, 0);
    fcntl(get_uinput_fd(), F_SETFL, flags | O_NONBLOCK);

    subscriber = std::make_shared<epoll_subscriber>(std::vector({get_uinput_fd()}),
                                                    [=](int event_fd){handle_events(event_fd);});
    epoll_manager.add_subscriber(subscriber);
}

virt_ctlr_combined::~virt_ctlr_combined()
{
    epoll_manager.remove_subscriber(subscriber);

    libevdev_uinput_destroy(uidev);
    close(uifd);
    libevdev_free(virt_evdev);
}

void virt_ctlr_combined::handle_events(int fd)
{
    if (physl && fd == physl->get_fd())
        relay_events(physl);
    else if (physr && fd == physr->get_fd())
        relay_events(physr);
    else if (fd == get_uinput_fd())
        handle_uinput_event();
    else
        std::cerr << "fd=" << fd << " is an invalid fd for this combined controller\n";
}

bool virt_ctlr_combined::contains_phys_ctlr(std::shared_ptr<phys_ctlr> const ctlr) const
{
    return physl == ctlr || physr == ctlr;
}

bool virt_ctlr_combined::contains_phys_ctlr(char const *devpath) const
{
    return (physl && physl->get_devpath() == devpath) || (physr && physr->get_devpath() == devpath);
}

bool virt_ctlr_combined::contains_fd(int fd) const
{
    return (physl && physl->get_fd() == fd) || (physr && physr->get_fd() == fd) ||
            libevdev_uinput_get_fd(uidev) == fd;
}

std::vector<std::shared_ptr<phys_ctlr>> virt_ctlr_combined::get_phys_ctlrs()
{
    std::vector<std::shared_ptr<phys_ctlr>> ctlrs;
    if (physl)
        ctlrs.push_back(physl);
    if (physr)
        ctlrs.push_back(physr);
    return ctlrs;
}

int virt_ctlr_combined::get_uinput_fd()
{
    return libevdev_uinput_get_fd(uidev);
}

void virt_ctlr_combined::remove_phys_ctlr(const std::shared_ptr<phys_ctlr> phys)
{
    if (phys == physl) {
        std::cout << "Removing left joy-con from virtual combined controller\n";
        physl = nullptr;
    } else if (phys == physr) {
        std::cout << "Removing right joy-con from virtual combined controller\n";
        physr = nullptr;
    } else {
        std::cerr << "ERROR: Attempted to remove non-existant controller from combined joy-cons\n";
        exit(EXIT_FAILURE);
    }
}

void virt_ctlr_combined::add_phys_ctlr(std::shared_ptr<phys_ctlr> phys)
{
    if (phys->get_model() == phys_ctlr::Model::Left_Joycon && !physl) {
        std::cout << "Re-adding left joy-con to virtual combined controller\n";
        physl = phys;
    } else if (phys->get_model() == phys_ctlr::Model::Right_Joycon && !physr) {
        std::cout << "Re-adding right joy-con to virtual combined controller\n";
        physr = phys;
    } else {
        std::cerr << "ERROR: Attempted to add invalid controller to combined joy-cons\n";
        exit(EXIT_FAILURE);
    }

    // re-add all the ff_effects to the reconnected controller
    for (auto& kv : rumble_effects) {
        struct ff_effect* effect;

        if (phys == physl)
            effect = &kv.second.first;
        else
            effect = &kv.second.second;
        effect->id = -1;

        if (ioctl(phys->get_fd(), EVIOCSFF, effect) == -1)
            std::cerr << "ERROR: Failed to reupload ff_ffect: " << strerror(errno) << std::endl;
    }
}

enum phys_ctlr::Model virt_ctlr_combined::needs_model()
{
    enum phys_ctlr::Model model = phys_ctlr::Model::Unknown;

    if (!physl)
        model = phys_ctlr::Model::Left_Joycon;
    else if (!physr)
        model = phys_ctlr::Model::Right_Joycon;
    return model;
}

bool virt_ctlr_combined::no_ctlrs_left()
{
    return !physl && !physr;
}
