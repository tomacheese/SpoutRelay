#pragma once
#include "common/types.hpp"
#include <memory>
#include <string>

class ISpoutMonitor {
public:
    virtual ~ISpoutMonitor() = default;

    virtual bool init(std::string& error) = 0;
    virtual bool probe_sender(const std::string& name) = 0;
    virtual bool connect(const std::string& name, std::string& error) = 0;
    virtual void disconnect() = 0;
    virtual bool receive_latest_frame(FrameBuffer& buf, FrameMeta& meta, bool& is_new) = 0;
    virtual SenderInfo get_sender_info() const = 0;
    virtual bool is_connected() const = 0;
};

class SpoutMonitor : public ISpoutMonitor {
public:
    SpoutMonitor();
    ~SpoutMonitor() override;

    bool init(std::string& error) override;
    bool probe_sender(const std::string& name) override;
    bool connect(const std::string& name, std::string& error) override;
    void disconnect() override;
    bool receive_latest_frame(FrameBuffer& buf, FrameMeta& meta, bool& is_new) override;
    SenderInfo get_sender_info() const override;
    bool is_connected() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
