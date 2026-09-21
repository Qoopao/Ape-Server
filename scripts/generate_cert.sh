# 0. 创建 cert 目录
mkdir -p cert && cd certs

# 1. 生成 CA 私钥 + CA 证书（CA:TRUE，有效期 10 年）
openssl req -x509 -newkey rsa:2048 \
    -keyout ca.key -out ca.crt \
    -days 3650 -nodes \
    -subj "/CN=MyTestCA" \
    -addext "basicConstraints=critical,CA:TRUE"

# 2. 生成服务器私钥 + CSR
openssl req -newkey rsa:2048 \
    -keyout server.key -out server.csr \
    -nodes \
    -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

# 3. 用 CA 签发服务器证书（有效期 1 年）
openssl x509 -req -in server.csr \
    -CA ca.crt -CAkey ca.key -CAcreateserial \
    -out server.crt -days 365 \
    -extfile <(printf "subjectAltName=DNS:localhost,IP:127.0.0.1")