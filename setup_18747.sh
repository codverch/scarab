#!/bin/bash
#==============================================================================
# Setup 18747
# Author: Deepanjali, Pragna, Shreesh, Claude
# Date: April 2, 2025
#==============================================================================

#------------------------------------------------------------------------------
# ANSI color codes for better readability
#------------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

#------------------------------------------------------------------------------
# Helper functions
#------------------------------------------------------------------------------
print_section() {
    echo -e "\n${BLUE}===========================================================================${NC}"
    echo -e "${BLUE}== $1${NC}"
    echo -e "${BLUE}===========================================================================${NC}"
}

print_subsection() {
    echo -e "\n${YELLOW}>> $1${NC}"
}

execute_cmd() {
    echo -e "${GREEN}Executing:${NC} $1"
    eval $1
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}Success!${NC}"
    else
        echo -e "${RED}Command failed with error code $?${NC}"
        echo -e "${RED}Please check the output above for more details.${NC}"
    fi
}

#------------------------------------------------------------------------------
# Initialize scarab workspace
#------------------------------------------------------------------------------
print_section "Initialize scarab workspace"
execute_cmd "../"
execute_cmd "mkdir -p scarab_ws"
execute_cmd "cd scarab_ws"

#------------------------------------------------------------------------------
# System preparation
#------------------------------------------------------------------------------
print_section "System Update and Package Installation"

print_subsection "Updating package lists and upgrading existing packages"
execute_cmd "sudo apt-get update && sudo apt-get upgrade -y"

print_subsection "Installing necessary development packages"
execute_cmd "sudo apt-get install -y g++-9 gcc-9 cmake zlib1g-dev libsnappy-dev libconfig++-dev libncurses5 python3-pip"

print_subsection "Installing Python packages"
execute_cmd "pip install gdown"

print_subsection "Setting up GCC and G++ alternatives"
execute_cmd "sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-9 100"
execute_cmd "sudo update-alternatives --install /usr/bin/g++ g++ /usr/bin/g++-9 100"

#------------------------------------------------------------------------------
# Clang Installation
#------------------------------------------------------------------------------
print_section "Installing LLVM/Clang 5.0.1"

print_subsection "Downloading Clang+LLVM"
execute_cmd "wget http://releases.llvm.org/5.0.1/clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz"

print_subsection "Extracting and installing Clang"
execute_cmd "tar -xf clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz"
execute_cmd "sudo rm -rf /usr/local/clang-5.0.1"
execute_cmd "sudo mkdir -p /usr/local/clang-5.0.1"
execute_cmd "sudo mv clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04/* /usr/local/clang-5.0.1/"
execute_cmd "rm clang+llvm-5.0.1-x86_64-linux-gnu-ubuntu-14.04.tar.xz"

#------------------------------------------------------------------------------
# Scarab and Pin Tool Installation
#------------------------------------------------------------------------------
print_section "Installing Scarab and Pin Tool"

echo "Which scarab config do you want to clone?"
echo "1) codverch (https://github.com/codverch/scarab.git)"
echo "2) hpsresearchgroup (https://github.com/hpsresearchgroup/scarab.git)"
read -p "Enter 1 or 2: " choice

print_subsection "Cloning Scarab repository"
if [ "$choice" == "1" ]; then
    execute_cmd "git clone https://github.com/codverch/scarab.git"
elif [ "$choice" == "2" ]; then
    execute_cmd "git clone https://github.com/hpsresearchgroup/scarab.git"
else
    echo "Invalid choice. Exiting."
    exit 1
fi

print_subsection "Downloading Intel Pin"
execute_cmd "wget http://software.intel.com/sites/landingpage/pintool/downloads/pin-3.15-98253-gb56e429b1-gcc-linux.tar.gz"
execute_cmd "tar -xzf pin-3.15-98253-gb56e429b1-gcc-linux.tar.gz"
execute_cmd "rm pin-3.15-98253-gb56e429b1-gcc-linux.tar.gz"
execute_cmd "mv pin-3.15-98253-gb56e429b1-gcc-linux pin-3.15"
execute_cmd "cp -r pin-3.15 scarab/src/"

#------------------------------------------------------------------------------
# Environment Configuration
#------------------------------------------------------------------------------
print_section "Setting up environment variables"

SCRIPT_DIR=$(pwd)
print_subsection "Adding environment variables to scarab.sh"
execute_cmd "echo \"export PIN_ROOT=$SCRIPT_DIR/scarab/src/pin-3.15\" >> scarab.sh"
execute_cmd "echo 'export PATH=\$PIN_ROOT/:\$PATH' >> scarab.sh"
execute_cmd "echo \"export SCARAB_ENABLE_PT_MEMTRACE=1\" >> scarab.sh"
execute_cmd "echo 'export PATH=/usr/local/clang-5.0.1/bin:\$PATH' >> scarab.sh"

#------------------------------------------------------------------------------
# Makefile Configuration
#------------------------------------------------------------------------------
print_section "Configuring Scarab makefile"

print_subsection "Fixing Pin tool makefile include path"
execute_cmd "sed -i 's|include \$(TOOLS_ROOT)/Config/makefile.default.rules|include \$(PIN_ROOT)/source/tools/Config/makefile.default.rules|g' scarab/src/pin/pin_exec/makefile"

#------------------------------------------------------------------------------
# Building Scarab
#------------------------------------------------------------------------------
print_section "Building Scarab"

print_subsection "Setting environment variables for the current session"
execute_cmd "export PIN_ROOT=$SCRIPT_DIR/scarab/src/pin-3.15"
execute_cmd "export PATH=$PIN_ROOT/:$PATH"
execute_cmd "export SCARAB_ENABLE_PT_MEMTRACE=1"
execute_cmd "export PATH=/usr/local/clang-5.0.1/bin:$PATH"

print_subsection "Verifying PIN_ROOT path"
execute_cmd "echo \"PIN_ROOT is set to: $PIN_ROOT\""
execute_cmd "ls -la $PIN_ROOT/source/tools/Config/ || echo \"Warning: Config directory not found as expected\""

print_subsection "Compiling Scarab"
execute_cmd "cd scarab/src && make PINPLAY_HOME=$PIN_ROOT PINPLAY_INCLUDE_HOME=$PIN_ROOT/source/tools/PinPlay TOOLS_ROOT=$PIN_ROOT/source/tools"

#------------------------------------------------------------------------------
# Setting up simpoint traces
#------------------------------------------------------------------------------
print_section "Setting up simpoint traces"

print_subsection "Creating directory and downloading traces"
execute_cmd "mkdir -p simpoint_traces"
execute_cmd "cd simpoint_traces"
execute_cmd "~/.local/bin/gdown 1tfKL7wYK1mUqpCH8yPaPVvxk2UIAJrOX"
execute_cmd "tar -xzvf simpoint_traces.tar.gz"
execute_cmd "rm -rf pt_*"
execute_cmd "cd .."

#------------------------------------------------------------------------------
# Setting up app traces
#------------------------------------------------------------------------------
print_section "Setting up app traces"
execute_cmd "~/.local/bin/gdown 1eiZ6ImJ9ZCfe83Vo5qJ94vdQw-BjR1ys"
execute_cmd "unzip traces.zip"
execute_cmd "rm traces.zip"

#------------------------------------------------------------------------------
# Final setup
#------------------------------------------------------------------------------
print_section "Finalizing installation"

execute_cmd "cd ../../"
print_subsection "Loading environment variables"
execute_cmd "source scarab.sh"

print_section "Installation Complete!"
echo -e "\n${GREEN}Scarab has been successfully installed and configured.${NC}"
echo -e "${GREEN}Please refer to the documentation for usage instructions.${NC}"
echo -e "${YELLOW}You may need to start a new terminal session or run 'source scarab.sh' to use Scarab.${NC}\n"

