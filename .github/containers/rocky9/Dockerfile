FROM rockylinux:9

RUN dnf -y group install 'Development Tools'
RUN dnf -y install dnf-plugins-core sudo
RUN dnf config-manager --set-enabled crb

WORKDIR /home
