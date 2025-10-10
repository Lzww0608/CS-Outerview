#include <iostream>    // 标准输入输出流，用于控制台打印
#include <fstream>     // 文件流操作，用于读取本地文件
#include <vector>      // 动态数组容器，用作数据缓冲区
#include <sstream>     // 字符串流，用于内存数据转换为流
#include <list>        // 链表容器，用于存储Multipart Upload的分块信息
#include <miniocpp/client.h>  // MinIO C++ SDK主要头文件

/**
 * MinIO 流模式上传示例程序
 * 
 * 功能说明：
 * 1. 从文件每次读取32KB数据到内存 - 模拟Web客户端分块上传行为
 * 2. 从内存中读取数据上传到MinIO - 模拟服务端接收并转发数据
 * 3. 使用Multipart Upload API将分块组合成完整文件 - 确保最终存储完整性
 * 4. 模拟边读取文件数据边上传的场景 - 流式处理，减少内存占用
 * 
 * 核心技术特点：
 * - 双路径处理：根据文件大小自动选择最优上传策略
 * - 内存友好：大文件处理时内存占用恒定（最大5MB+32KB）
 * - 完整性保证：确保MinIO中存储的是完整文件而非分块文件
 * - Web场景模拟：真实模拟Web分块上传的服务端处理逻辑
 * 
 * 依赖库：
 * - MinIO C++ SDK - 对象存储操作
 * 
 */

int main(int argc, char* argv[]) {
    // ==================== 命令行参数验证 ====================
    // 检查用户是否提供了正确的命令行参数
    if (argc != 2) {
        std::cerr << "使用方法: " << argv[0] << " <source_file>" << std::endl;
        return 1;
    }
    std::string sourceFile = argv[1];  // 获取要上传的源文件路径

    // ==================== MinIO服务器连接配置 ====================
    // 配置MinIO服务器连接参数，根据实际环境修改
    std::string minioEndpoint = "localhost:9000";  // MinIO服务器地址和端口
    std::string accessKey = "minioadmin";           // 访问密钥ID
    std::string secretKey = "minioadmin";           // 秘密访问密钥
    bool useSSL = false;                            // 是否使用SSL/TLS加密连接

    // 创建MinIO客户端实例
    // 1. 创建基础URL对象，封装服务器地址和协议信息
    minio::s3::BaseUrl baseUrl(minioEndpoint, useSSL);
    // 2. 创建静态凭证提供者，管理访问密钥
    minio::creds::StaticProvider provider(accessKey, secretKey);
    // 3. 创建MinIO客户端，这是所有操作的核心接口
    minio::s3::Client minio(baseUrl, &provider);

    // ==================== 流模式上传配置 ====================
    // 定义上传目标和分块参数
    std::string bucketName = "video";                       // 目标存储桶名称
    std::string objectName = sourceFile;                    // 对象名称（使用源文件名）
    const size_t CHUNK_SIZE = 32 * 1024;                   // 32KB - 模拟Web客户端分块大小
    const size_t MIN_PART_SIZE = 5 * 1024 * 1024;          // 5MB - MinIO Multipart Upload最小分块要求
    
    try {
        // ==================== 文件存在性检查 ====================
        // 在开始处理前确保源文件存在且可访问
        std::ifstream checkFile(sourceFile);
        if (!checkFile.is_open()) {
            std::cerr << "源文件不存在: " << sourceFile << std::endl;
            return 1;
        }
        checkFile.close();
        
        // ==================== 文件大小获取 ====================
        // 获取文件总大小，用于后续的处理策略选择和进度显示
        // 使用ios::ate标志将文件指针定位到文件末尾，便于获取文件大小
        std::ifstream sizeCheckFile(sourceFile, std::ios::binary | std::ios::ate);
        size_t totalSize = sizeCheckFile.tellg();  // tellg()返回当前位置，即文件大小
        sizeCheckFile.close();
        
        // ==================== 开始处理提示信息 ====================
        // 显示当前处理任务的详细信息
        std::cout << "=== 开始模拟web上传流式传输 ===" << std::endl;
        std::cout << "源文件: " << sourceFile << std::endl;
        std::cout << "目标位置: " << bucketName << "/" << objectName << std::endl;
        std::cout << "每次读取: " << CHUNK_SIZE << " 字节 (" << CHUNK_SIZE / 1024 << "KB)" << std::endl;
        std::cout << "文件总大小: " << totalSize << " 字节 (" << totalSize / 1024 << "KB)" << std::endl;
        
        // ==================== 处理策略选择：根据文件大小决定上传方式 ====================
        if (totalSize < MIN_PART_SIZE) {
            // ==================== 小文件处理路径（< 5MB）====================
            // 策略：先将整个文件按32KB分块读取到内存，然后一次性上传
            // 优点：实现简单，适合小文件；缺点：内存占用等于文件大小
            std::cout << "\n文件小于5MB，使用普通PutObject上传..." << std::endl;
            
            // 打开源文件用于读取，使用二进制模式避免文本模式的换行符转换
            std::ifstream file(sourceFile, std::ios::binary);
            std::vector<char> allData;           // 总数据缓冲区，存储整个文件内容
            std::vector<char> buffer(CHUNK_SIZE); // 32KB读取缓冲区，用于分块读取
            size_t totalRead = 0;                // 已读取的总字节数，用于进度跟踪
            
            // ==================== 分块读取阶段 ====================
            // 按32KB块循环读取文件，模拟Web客户端分块上传的数据接收过程
            while (file.read(buffer.data(), CHUNK_SIZE) || file.gcount() > 0) {
                size_t bytesRead = file.gcount();  // 获取实际读取的字节数（最后一块可能不足32KB）
                totalRead += bytesRead;            // 累计已读取字节数
                
                // 显示读取进度，模拟服务端接收Web客户端数据的过程
                std::cout << "从文件读取到内存: " << bytesRead << " 字节 (总计: " 
                          << totalRead << "/" << totalSize << ")" << std::endl;
                
                // 将当前读取的数据追加到总缓冲区
                // 注意：只追加实际读取的字节数，避免添加未使用的缓冲区空间
                allData.insert(allData.end(), buffer.begin(), buffer.begin() + bytesRead);
            }
            file.close();  // 关闭文件，释放文件句柄
            
            std::cout << "所有数据已读取到内存，开始从内存上传到MinIO..." << std::endl;
            
            // ==================== 内存数据转换和上传阶段 ====================
            // 将vector<char>转换为string，再创建istringstream供MinIO SDK使用
            std::string dataStr(allData.begin(), allData.end());  // 转换为字符串
            std::istringstream dataStream(dataStr);               // 创建字符串输入流
            
            // 创建PutObject参数对象
            // 参数说明：数据流、文件大小、分块大小（0表示让SDK自动处理）
            minio::s3::PutObjectArgs args(dataStream, allData.size(), 0);
            args.bucket = bucketName;  // 设置目标存储桶
            args.object = objectName;  // 设置目标对象名称
            
            // 执行上传操作
            minio::s3::PutObjectResponse resp = minio.PutObject(args);
            if (!resp) {
                std::cerr << "文件上传失败: " << resp.Error().String() << std::endl;
                return 1;
            }
            
            // ==================== 小文件上传结果显示 ====================
            std::cout << "\n=== 小文件上传完成 ===" << std::endl;
            std::cout << "文件上传成功！" << std::endl;
            std::cout << "ETag: " << (resp.etag.empty() ? "无" : resp.etag) << std::endl;
            
        } else {
            // ==================== 大文件处理路径（>= 5MB）====================
            // 策略：使用MinIO Multipart Upload API，按32KB读取并累积到5MB后分块上传
            // 优点：内存占用恒定（最大5MB+32KB），支持超大文件；缺点：实现复杂
            std::cout << "\n文件大于等于5MB，使用Multipart Upload..." << std::endl;
            
            // ==================== 步骤1：初始化Multipart Upload会话 ====================
            // 创建Multipart Upload会话，获取upload_id用于后续所有分块操作
            minio::s3::CreateMultipartUploadArgs createArgs;
            createArgs.bucket = bucketName;  // 设置目标存储桶
            createArgs.object = objectName;  // 设置目标对象名称
            
            // 发起Multipart Upload创建请求
            minio::s3::CreateMultipartUploadResponse createResp = minio.CreateMultipartUpload(createArgs);
            if (!createResp) {
                std::cerr << "创建Multipart Upload失败: " << createResp.Error().String() << std::endl;
                return 1;
            }
            
            // 获取上传会话ID，所有后续的分块上传都需要这个ID
            std::string uploadId = createResp.upload_id;
            std::cout << "Multipart Upload创建成功，Upload ID: " << uploadId << std::endl;
            
            // ==================== 步骤2：分块读取和上传循环 ====================
            // 打开文件进行分块读取处理
            std::ifstream file(sourceFile, std::ios::binary);
            std::list<minio::s3::Part> parts;                   // 已完成分块列表，用于最终合并
            std::vector<char> partBuffer;                       // 分块累积缓冲区，最大5MB
            std::vector<char> readBuffer(CHUNK_SIZE);           // 32KB读取缓冲区，固定大小
            size_t totalRead = 0;                              // 已读取的总字节数
            int partNumber = 1;                                // 分块编号，从1开始
            
            // 主循环：按32KB读取文件，累积到5MB后上传分块
            while (file.read(readBuffer.data(), CHUNK_SIZE) || file.gcount() > 0) {
                size_t bytesRead = file.gcount();  // 获取实际读取字节数
                totalRead += bytesRead;            // 累计总读取字节数
                
                // 显示读取进度，模拟服务端接收Web客户端分块数据
                std::cout << "从文件读取到内存: " << bytesRead << " 字节 (总计: " 
                          << totalRead << "/" << totalSize << ")" << std::endl;
                
                // 将当前读取的32KB数据追加到分块缓冲区
                partBuffer.insert(partBuffer.end(), readBuffer.begin(), readBuffer.begin() + bytesRead);
                
                // ==================== 分块上传条件判断 ====================
                // 条件1：缓冲区达到5MB（MinIO最小分块要求）
                // 条件2：文件读取完毕（处理最后一个可能不足5MB的分块）
                bool isLastPart = (totalRead >= totalSize);
                if (partBuffer.size() >= MIN_PART_SIZE || isLastPart) {
                    std::cout << "从内存上传分块 " << partNumber << " 到MinIO，大小: " 
                              << partBuffer.size() << " 字节" << std::endl;
                    
                    // 将数据从vector转换为string，然后创建string_view
                    std::string partDataStr(partBuffer.begin(), partBuffer.end());
                    std::string_view partData(partDataStr);
                    
                    minio::s3::UploadPartArgs uploadPartArgs;
                    uploadPartArgs.bucket = bucketName;
                    uploadPartArgs.object = objectName;
                    uploadPartArgs.upload_id = uploadId;
                    uploadPartArgs.part_number = partNumber;
                    uploadPartArgs.data = partData;
                    
                    // 立即上传，确保partDataStr在作用域内
                    minio::s3::UploadPartResponse uploadPartResp = minio.UploadPart(uploadPartArgs);
                    if (!uploadPartResp) {
                        std::cerr << "分块 " << partNumber << " 上传失败: " 
                                  << uploadPartResp.Error().String() << std::endl;
                        return 1;
                    }
                    
                    // 记录完成的分块
                    minio::s3::Part part;
                    part.number = partNumber;
                    part.etag = uploadPartResp.etag;
                    parts.push_back(part);
                    
                    std::cout << "分块 " << partNumber << " 上传成功，ETag: " 
                              << uploadPartResp.etag << std::endl;
                    
                    // 清空缓冲区，准备下一个分块
                    partBuffer.clear();
                    partNumber++;
                }
            }
            file.close();
            
            // 步骤3：完成Multipart Upload
            std::cout << "\n完成Multipart Upload..." << std::endl;
            minio::s3::CompleteMultipartUploadArgs completeArgs;
            completeArgs.bucket = bucketName;
            completeArgs.object = objectName;
            completeArgs.upload_id = uploadId;
            completeArgs.parts = parts;
            
            minio::s3::CompleteMultipartUploadResponse completeResp = minio.CompleteMultipartUpload(completeArgs);
            if (!completeResp) {
                std::cerr << "完成Multipart Upload失败: " << completeResp.Error().String() << std::endl;
                return 1;
            }
            
            std::cout << "\n=== 大文件上传完成 ===" << std::endl;
            std::cout << "文件上传成功！" << std::endl;
            std::cout << "总分块数: " << parts.size() << std::endl;
            std::cout << "最终ETag: " << completeResp.etag << std::endl;
            std::cout << "文件位置: " << completeResp.location << std::endl;
        }
        
        std::cout << "\n注意：文件已成功上传为完整文件。" << std::endl;
        std::cout << "这模拟了从文件读取32KB数据到内存，然后从内存上传到MinIO的场景。" << std::endl;
        
    } catch (const std::exception& e) {
        std::cerr << "流模式上传失败: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n=== 程序执行完成 ===" << std::endl;
    return 0;
} 