project_root="$(dirname $0)"
echo "vcpkg install dependencies in $project_root/vcpkg_installed"
vcpkg install --triplet x64-linux