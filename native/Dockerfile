FROM rockylinux:8

# Basis build tools
RUN yum install -y epel-release && \
    yum groupinstall -y "Development Tools"

# CMake3 (Rocky 8 heeft cmake 3.18, maar cmake3 is 3.20+)
RUN yum install -y cmake3

# Qt5 development packages
RUN yum install -y \
    qt5-qtbase-devel \
    qt5-qttools-devel \
    qt5-qtsvg-devel

# Extra libraries die jouw project gebruikt
RUN yum install -y openssl-devel

# Zorg dat cmake3 de default is
RUN ln -s /usr/bin/cmake3 /usr/local/bin/cmake

# Werkdirectory
WORKDIR /src

# Default command: open a shell
CMD ["/bin/bash"]
