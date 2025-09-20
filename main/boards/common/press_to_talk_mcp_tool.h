#ifndef PRESS_TO_TALK_MCP_TOOL_H
#define PRESS_TO_TALK_MCP_TOOL_H

#include "mcp_server.h"
#include "settings.h"

// Reusable push-to-talk mode MCP tool class
class PressToTalkMcpTool {
private:
    bool press_to_talk_enabled_;

public:
    PressToTalkMcpTool();
    
    // Initialize the tool and register with the MCP server
    void Initialize();
    
    // Get the current push-to-talk mode status
    bool IsPressToTalkEnabled() const;

private:
    // MCP tool callback function
    ReturnValue HandleSetPressToTalk(const PropertyList& properties);
    
    // Internal method: Set the press to talk state and save it to settings
    void SetPressToTalkEnabled(bool enabled);
};

#endif // PRESS_TO_TALK_MCP_TOOL_H 