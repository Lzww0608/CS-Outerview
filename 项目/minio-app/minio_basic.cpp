#include <iostream>
#include <fstream>
#include <miniocpp/client.h>
#include <miniocpp/providers.h>

/**
 * MinIO C++ 客户端示例程序
 * 
 * 使用前需要先设置MinIO服务器：
 * 1. 下载并安装MinIO服务器：https://min.io/download
 * 2. 启动MinIO服务器：
 *    - Linux/Mac: ./minio server /path/to/data
 *    - Windows: minio.exe server C:\path\to\data
 * 3. 默认访问地址：http://localhost:9000
 * 4. 默认管理员账号：minioadmin / minioadmin
 * 
 * 编译时需要链接MinIO C++ SDK：
 * g++ -o minio_app minio.cpp -lminio
 */

int main(int argc, char *argv[]) {
     // 本地文件路径.
    // 要上传到MinIO的本地文件的完整路径
    std::string filePath = "test-file.txt";
    // ==================== 命令行参数验证 ====================
    // 检查用户是否提供了正确的命令行参数
    if (argc == 2) {
        filePath = argv[1];  // 获取要上传的源文件路径
    }
   

    // ==================== MinIO服务器连接配置 ====================
    // 注意：请根据你的实际MinIO服务器配置修改以下参数
    
    // MinIO服务器地址和端口
    // 本地部署：localhost:9000
    // 远程服务器：your-server-ip:9000
    // 使用HTTPS：your-server-ip:9000
    std::string minioEndpoint = "localhost:9000";
    
    // 访问密钥ID (Access Key ID)
    // 在MinIO控制台 -> Identity -> Users 中创建用户获取
    // 默认管理员账号的Access Key是：minioadmin
    std::string accessKey = "minioadmin";
    
    // 秘密访问密钥 (Secret Access Key)
    // 在MinIO控制台 -> Identity -> Users 中创建用户获取
    // 默认管理员账号的Secret Key是：minioadmin
    std::string secretKey = "minioadmin";
    
    // 是否使用SSL/TLS加密连接
    // true: 使用HTTPS (https://)
    // false: 使用HTTP (http://)
    bool useSSL = false;

    // ==================== 创建MinIO客户端 ====================
    // 初始化MinIO客户端对象，建立与服务器的连接
    minio::s3::BaseUrl baseUrl(minioEndpoint, useSSL);
    minio::creds::StaticProvider provider(accessKey, secretKey);
    minio::s3::Client minio(baseUrl, &provider);

    // ==================== 文件上传配置 ====================
    // 存储桶名称 (Bucket Name)
    // 存储桶是MinIO中存储对象的容器，类似于文件夹
    // 如果存储桶不存在，需要先在MinIO控制台创建
    std::string bucketName = "video";
    
    // 对象名称 (Object Name)
    // 这是文件在MinIO中的存储名称，可以包含路径
    // 例如："documents/report.pdf" 或 "images/photo.jpg"
    std::string objectName = filePath;
    
    

    // ==================== 执行文件上传 ====================
    std::cout << "开始上传文件到MinIO..." << std::endl;
    try {
        // 上传文件到MinIO服务器
        // 使用文件流上传
        std::ifstream fileStream(filePath, std::ios::binary);
        if (!fileStream.is_open()) {
            std::cerr << "无法打开文件: " << filePath << std::endl;
            return 1;
        }
        
        // 获取文件大小
        fileStream.seekg(0, std::ios::end);
        long fileSize = fileStream.tellg();
        fileStream.seekg(0, std::ios::beg);
        
        // 创建上传参数
        minio::s3::PutObjectArgs args(fileStream, fileSize, 0);
        args.bucket = bucketName;
        args.object = objectName;
        
        // 执行上传
        minio::s3::PutObjectResponse resp = minio.PutObject(args);
        if (!resp) {
            std::cerr << "上传失败: " << resp.Error().String() << std::endl;
            return 1;
        }
        std::string etag = resp.headers.GetFront("etag"); // 获取 ETag
        
        std::cout << "文件上传成功, ETag: " << etag << std::endl;
        std::cout << "存储位置: " << bucketName << "/" << objectName << std::endl;
        
        // 打印详细的响应信息
        std::cout << "\n=== 上传响应详细信息 ===" << std::endl;
        std::cout << "状态码: " << resp.status_code << std::endl;
        std::cout << "ETag: " << (resp.etag.empty() ? "无" : resp.etag) << std::endl;
        std::cout << "版本ID: " << (resp.version_id.empty() ? "无" : resp.version_id) << std::endl;
        std::cout << "请求ID: " << (resp.request_id.empty() ? "无" : resp.request_id) << std::endl;
        std::cout << "主机ID: " << (resp.host_id.empty() ? "无" : resp.host_id) << std::endl;
        
        // 打印响应头信息
        if (resp.headers) {
            std::cout << "响应头信息:" << std::endl;
            for (const auto& key : resp.headers.Keys()) {
                std::list<std::string> values = resp.headers.Get(key);
                for (const auto& value : values) {
                    std::cout << "  " << key << ": " << value << std::endl;
                }
            }
        }
        fileStream.close();
    } catch (const std::exception& e) {
        std::cerr << "上传文件时发生错误: " << e.what() << std::endl;
        std::cerr << "请检查：" << std::endl;
        std::cerr << "1. MinIO服务器是否正在运行" << std::endl;
        std::cerr << "2. 连接参数是否正确" << std::endl;
        std::cerr << "3. 存储桶是否存在" << std::endl;
        std::cerr << "4. 本地文件是否存在" << std::endl;
        return 1;
    }

    // ==================== 文件下载配置 ====================
    // 下载文件的保存路径
    // 指定下载文件要保存到本地的位置
    std::string downloadPath = "downloaded-"+filePath;

    // ==================== 执行文件下载 ====================
    std::cout << "开始从MinIO下载文件..." << std::endl;
    try {
        // 从MinIO服务器下载文件
        // 使用回调函数接收数据
        minio::s3::GetObjectArgs args;
        args.bucket = bucketName;
        args.object = objectName;
        
        // 创建输出文件流
        std::ofstream outFile(downloadPath, std::ios::binary);
        if (!outFile.is_open()) {
            std::cerr << "无法创建输出文件: " << downloadPath << std::endl;
            return 1;
        }
        
        // 设置数据回调函数
        args.datafunc = [&outFile](minio::http::DataFunctionArgs dataArgs) -> bool {
            outFile.write(dataArgs.datachunk.c_str(), dataArgs.datachunk.length());
            return true;
        };
        
        // 执行下载
        minio::s3::GetObjectResponse resp = minio.GetObject(args);
        if (!resp) {
            std::cerr << "下载失败: " << resp.Error().String() << std::endl;
            outFile.close();
            return 1;
        }
        
        outFile.close();
        std::cout << "文件下载成功！" << std::endl;
        std::cout << "保存位置: " << downloadPath << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "下载文件时发生错误: " << e.what() << std::endl;
        std::cerr << "请检查：" << std::endl;
        std::cerr << "1. 文件是否存在于MinIO中" << std::endl;
        std::cerr << "2. 本地保存路径是否可写" << std::endl;
        std::cerr << "3. 网络连接是否正常" << std::endl;
        return 1;
    }

    std::cout << "程序执行完成！" << std::endl;
    return 0;
}
