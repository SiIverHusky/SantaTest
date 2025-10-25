#ifndef DUAL_NETWORK_BOARD_H
#define DUAL_NETWORK_BOARD_H

#include "board.h"
#include "wifi_board.h"
#include "ml307_board.h"
#include <memory>

//enum NetworkType
enum class NetworkType {
    WIFI,
    ML307
};

// Dual network board class, can switch between WiFi and ML307
class DualNetworkBoard : public Board {
private:
    // Use base class pointer to store current active board
    std::unique_ptr<Board> current_board_;
    NetworkType network_type_ = NetworkType::ML307;  // Default to ML307

    // ML307 pin configuration
    gpio_num_t ml307_tx_pin_;
    gpio_num_t ml307_rx_pin_;
    gpio_num_t ml307_dtr_pin_;
    
    // Load network type from Settings
    NetworkType LoadNetworkTypeFromSettings(int32_t default_net_type);
    
    // Save network type to Settings
    void SaveNetworkTypeToSettings(NetworkType type);

    // Initialize the board corresponding to current network type
    void InitializeCurrentBoard();
 
public:
    DualNetworkBoard(gpio_num_t ml307_tx_pin, gpio_num_t ml307_rx_pin, gpio_num_t ml307_dtr_pin = GPIO_NUM_NC, int32_t default_net_type = 1);
    virtual ~DualNetworkBoard() = default;
 
    // Switch network type
    void SwitchNetworkType();
    
    // Get current network type
    NetworkType GetNetworkType() const { return network_type_; }
    
    // Get reference to current active board
    Board& GetCurrentBoard() const { return *current_board_; }
    
    // Rewrite the Board interface
    virtual std::string GetBoardType() override;
    virtual void StartNetwork() override;
    virtual NetworkInterface* GetNetwork() override;
    virtual const char* GetNetworkStateIcon() override;
    virtual void SetPowerSaveMode(bool enabled) override;
    virtual std::string GetBoardJson() override;
    virtual std::string GetDeviceStatusJson() override;
};

#endif // DUAL_NETWORK_BOARD_H 